#include "display.h"
#include "cache.h"
#include <algorithm>
#include <cstring>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

extern volatile bool gHoldRequested;

// -------- EPD & buffers --------
Epd epd;
unsigned char full_image[400 / 8 * 300];
Paint full_paint(full_image, 400, 300);

unsigned char image[400 / 8 * 28];
Paint paint(image, 400, 28);

// -------- Display mutex --------
static SemaphoreHandle_t gDisplayMutex = nullptr;
void initDisplayMutex() {
  if (!gDisplayMutex) gDisplayMutex = xSemaphoreCreateMutex();
}
struct ScopedDispLock {
  bool locked{false};
  ScopedDispLock(TickType_t to = portMAX_DELAY) {
    if (gDisplayMutex) locked = (xSemaphoreTake(gDisplayMutex, to) == pdTRUE);
    else locked = true;
  }
  ~ScopedDispLock() { if (gDisplayMutex && locked) xSemaphoreGive(gDisplayMutex); }
};

// -------- State & layout --------
static DisplayMode sMode = LIVE_MODE;
static int  sCurrentPage = 1;       // 1-indexed
static int  sTotalPages  = 1;
static int  sTotalActive = 0;
static char sLastTimeStr[64] = "";

static constexpr int kScreenW = 400;
static constexpr int kScreenH = 300;
static constexpr int kLineH   = 27;

// compact snapshot — no Arduino String on heap in draw path
struct RowView {
  char   icao24[9];     // 8 + NUL
  char   model[48];     // trimmed
  char   callsign[9];   // 8 + NUL
  char   country[32];   // trimmed
  float  distance;
  float  bearing;
  bool   active;
};

// in display.cpp (near top)
static void DrawBitmap1bpp(Paint& p, int x, int y,
                           const uint8_t* data, int w, int h,
                           int color) {
  const int stride = (w + 7) / 8;
  for (int j = 0; j < h; ++j) {
    const uint8_t* row = data + j * stride;
    uint8_t mask = 0x80; int byte = 0;
    for (int i = 0; i < w; ++i) {
      if (row[byte] & mask) p.DrawAbsolutePixel(x + i, y + j, color);
      mask >>= 1; if (!mask) { mask = 0x80; ++byte; }
    }
  }
}

static int countActiveAircraft() {
  int cnt = 0;
  for (int i = 0; i < MAX_CACHE_SIZE; i++) {
    if (aircraftCache[i].icao24 != "" && aircraftCache[i].distance >= 0) cnt++;
  }
  return cnt;
}

int getActiveCount() {          // <--- NEW (exposed in .h)
  return countActiveAircraft();
}
bool hasActiveAircraft() {
  return getActiveCount() >= 5;   // must have at least 5 to enter HOLD
}
// safe copy helper
static void cpyBound(char* dst, size_t dstsz, const String& src) {
  if (!dst || dstsz == 0) return;
  size_t n = src.length();
  if (n >= dstsz) n = dstsz - 1;
  memcpy(dst, src.c_str(), n);
  dst[n] = '\0';
}

// get 2-letter compass suffix without heap allocs
static void compass2(float bearing, char out[3]) {
  // N, NE, E, SE, S, SW, W, NW
  static const char table[8][3] = {"N ","NE","E ","SE","S ","SW","W ","NW"};
  int idx = (int)lroundf(bearing / 45.0f);
  if (idx < 0) idx = 0;
  if (idx > 8) idx = 8;
  if (idx == 8) idx = 0; // 360 -> N
  out[0] = table[idx][0];
  out[1] = table[idx][1];
  out[2] = '\0';
}

// Copy active rows while holding cache mutex briefly
static int snapshotActiveRows(RowView* out, int maxOut) {
  int n = 0;
  if (!gCacheMutex || xSemaphoreTake(gCacheMutex, pdMS_TO_TICKS(50)) != pdTRUE) return 0;
  for (int i = 0; i < MAX_CACHE_SIZE && n < maxOut; i++) {
    const auto& e = aircraftCache[i];
    if (e.icao24 != "" && e.distance >= 0 && e.active) {
      cpyBound(out[n].icao24,  sizeof(out[n].icao24),  e.icao24);
      cpyBound(out[n].model,   sizeof(out[n].model),   e.model);
      cpyBound(out[n].callsign,sizeof(out[n].callsign),e.callsign);
      cpyBound(out[n].country, sizeof(out[n].country), e.country);
      out[n].distance = e.distance;
      out[n].bearing  = e.bearing;
      out[n].active   = true;
      n++;
    }
  }
  xSemaphoreGive(gCacheMutex);
  return n;
}

static void recalcPagingFromActive(int activeCount) {
  sTotalActive = activeCount;
  sTotalPages  = std::max(1, (sTotalActive + 5 - 1) / 5);
  if (sCurrentPage > sTotalPages) sCurrentPage = sTotalPages;
  if (sCurrentPage < 1) sCurrentPage = 1;
}

// -------- Status screens (locked) --------
void drawStatusScreen(const char* msg) {
  ScopedDispLock _;
  full_paint.Clear(UNCOLORED);
  epd.Init();
  epd.Clear();

  sFONT* font = &Font16;
  int16_t w = (int16_t)strlen(msg) * font->Width;
  int16_t h = font->Height;
  int16_t x = (kScreenW - w) / 2;
  int16_t y = (kScreenH - h) / 2;

  full_paint.DrawStringAt(x, y, msg, font, COLORED);
  epd.Display(full_paint.GetImage());
}

void drawStatusScreenwithline(const char* line1,
                              const char* line2,
                              const char* line3,
                              const char* line4,
                              const char* line5,
                              const char* line6) {
  ScopedDispLock _;
  full_paint.Clear(UNCOLORED);
  epd.Init_Fast(0);
  epd.Clear();

  sFONT* font = &Font16;
  int lineHeight = font->Height + 4;
  const int totalLines = 6;
  int startY = (kScreenH - (lineHeight * totalLines)) / 2;

  const char* lines[] = {line1, line2, line3, line4, line5, line6};
  for (int i = 0; i < totalLines; i++) {
    if (lines[i] && strlen(lines[i]) > 0) {
      full_paint.DrawStringAt(10, startY + i * lineHeight, lines[i], font, COLORED);
    }
  }

  epd.Display(full_paint.GetImage());
}

void drawNoAircraftScreen(time_t timestamp) {
  ScopedDispLock _;

  // Apply timezone offset and format strings
  time_t zoned = timestamp + (TZ_minutes * 60);
  struct tm* ti = gmtime(&zoned);

  char dateStr[32];
  strftime(dateStr, sizeof(dateStr), "%a %Y-%m-%d", ti);

  char clockStr[16];
  if (USE_24H) strftime(clockStr, sizeof(clockStr), "%H:%M", ti);
  else         strftime(clockStr, sizeof(clockStr), "%I:%M %p", ti);

  int currentY = 0;

  // --- Date (write-only, no refresh yet) ---
  paint.Clear(UNCOLORED);
  int dateX = (kScreenW - (int)strlen(dateStr) * Font16.Width) / 2;
  paint.DrawStringAt(dateX, 5, dateStr, &Font16, COLORED);
  epd.Display_Partial_Not_refresh(paint.GetImage(), 0, currentY, kScreenW, currentY + kLineH);
  currentY += kLineH;

  // --- Time (centered) (write-only) ---
  int clockY = (kScreenH - Font24.Height) / 2;
  paint.Clear(UNCOLORED);
  int clockX = (kScreenW - (int)strlen(clockStr) * Font24.Width) / 2;
  paint.DrawStringAt(clockX,     5, clockStr, &Font24, COLORED);
  paint.DrawStringAt(clockX + 1, 5, clockStr, &Font24, COLORED);  // fake bold
  epd.Display_Partial_Not_refresh(paint.GetImage(), 0, clockY, kScreenW, clockY + kLineH);

  // --- Footer (write-only) ---
  const char* footer = "no aircraft nearby";
  int footerY = kScreenH - kLineH;
  paint.Clear(UNCOLORED);
  int footerX = (kScreenW - (int)strlen(footer) * Font16.Width) / 2;
  paint.DrawStringAt(footerX, 5, footer, &Font16, COLORED);
  epd.Display_Partial_Not_refresh(paint.GetImage(), 0, footerY, kScreenW, footerY + kLineH);

  // --- Single partial refresh to apply all three bands ---
  epd.TurnOnDisplay_Partial();
}

// -------- HOLD (single FULL refresh, zebra stripes) --------
// static void drawHoldPageFullBuffer() {
//   // 1) Snapshot + sort
//   RowView rows[MAX_CACHE_SIZE];
//   int active = snapshotActiveRows(rows, MAX_CACHE_SIZE);
//   std::sort(rows, rows + active, [](const RowView& a, const RowView& b){
//     return a.distance < b.distance;
//   });
//   recalcPagingFromActive(active);

//   ScopedDispLock _;

//   // 2) Build HH:MM from the same source LIVE uses (sLastTimeStr)
//   //    LIVE sets sLastTimeStr to formats like "YYYY-MM-DD | HH:MM:SS" (24h)
//   //    or "YYYY-MM-DD | HH:MM:SS AM/PM" (12h). We parse HH:MM robustly.
//   char hhmm[6] = "--:--";
//   if (sLastTimeStr[0]) {
//     const char* p = strchr(sLastTimeStr, '|');  // find first '|'
//     if (p) {
//       p++; // move past '|'
//       while (*p && !isdigit((unsigned char)*p)) p++; // advance to first digit
//       int H = 0, M = 0;
//       if (sscanf(p, "%2d:%2d", &H, &M) == 2) {
//         if (H < 0) H = 0; if (H > 23) H = H % 24;
//         if (M < 0) M = 0; if (M > 59) M = M % 60;
//         snprintf(hhmm, sizeof(hhmm), "%02d:%02d", H, M);
//       }
//     }
//   }
//   // Fallback to local clock if LIVE hasn't populated sLastTimeStr yet
//   if (hhmm[0] == '-' && hhmm[1] == '-') {
//     time_t now = time(nullptr);
//     struct tm ti;
//     localtime_r(&now, &ti);
//     snprintf(hhmm, sizeof(hhmm), "%02d:%02d", ti.tm_hour, ti.tm_min);
//   }

//   // 3) Compose full frame (white text on black background)
//   full_paint.Clear(COLORED);

//   char header[64];
//   snprintf(header, sizeof(header), "HOLD | %s | PAGE %d/%d | TOTAL:%d",
//            hhmm, sCurrentPage, sTotalPages, sTotalActive);
//   full_paint.DrawStringAt(0, 5, header, &Font16, UNCOLORED);

//   int y = kLineH;
//   const int maxShown = 5;
//   const int start    = (sCurrentPage - 1) * maxShown;

//   int shown = 0;
//   for (int i = start; i < active && shown < maxShown; i++) {
//     const auto& r = rows[i];

//     // Callsign (fallback to ICAO), safe fixed-width fields
//     char callsign[9];
//     if (r.callsign[0] == '\0') { strncpy(callsign, r.icao24, sizeof(callsign)); callsign[sizeof(callsign)-1] = '\0'; }
//     else                       { strncpy(callsign, r.callsign, sizeof(callsign)); callsign[sizeof(callsign)-1] = '\0'; }

//     char csPadded[8];  snprintf(csPadded, sizeof(csPadded), "%-7.7s", callsign);

//     float dist = r.distance; if (dist > 99999.9f) dist = 99999.9f;
//     char distPadded[12]; snprintf(distPadded, sizeof(distPadded), "%7.1f", dist);

//     int  bInt = (int)r.bearing; if (bInt < 0) bInt += 360; if (bInt > 359) bInt -= 360;
//     char brg[4];  snprintf(brg, sizeof(brg), "%3d", bInt);

//     char cd[3];   compass2(r.bearing, cd);

//     char infoLine[96];
//     snprintf(infoLine, sizeof(infoLine), "%s%skm %s%s %s",
//              csPadded, distPadded, brg, cd, r.country);

//     // Model line — white on black
//     full_paint.DrawStringAt(5, y + 5, r.model, &Font16, UNCOLORED);
//     y += kLineH;

//     // Info line — white on black
//     full_paint.DrawStringAt(5, y + 5, infoLine, &Font16, UNCOLORED);
//     y += kLineH;

//     shown++;
//   }

//   // 4) Push once (fast full-frame)
//   epd.Init_Fast(0);
//   epd.Display(full_paint.GetImage());
// }


static void drawHoldPageFullBuffer() {
  RowView rows[MAX_CACHE_SIZE];
  int active = snapshotActiveRows(rows, MAX_CACHE_SIZE);
  std::sort(rows, rows + active, [](const RowView& a, const RowView& b){
    return a.distance < b.distance;
  });
  recalcPagingFromActive(active);

  ScopedDispLock _;

  char hhmm[6] = "--:--";
  if (sLastTimeStr[0]) {
    const char* p = strchr(sLastTimeStr, '|');  // find first '|'
    if (p) {
      p++; // move past '|'
      while (*p && !isdigit((unsigned char)*p)) p++; // advance to first digit
      int H = 0, M = 0;
      if (sscanf(p, "%2d:%2d", &H, &M) == 2) {
        if (H < 0) H = 0; if (H > 23) H = H % 24;
        if (M < 0) M = 0; if (M > 59) M = M % 60;
        snprintf(hhmm, sizeof(hhmm), "%02d:%02d", H, M);
      }
    }
  }
  // Fallback to local clock if LIVE hasn't populated sLastTimeStr yet
  if (hhmm[0] == '-' && hhmm[1] == '-') {
    time_t now = time(nullptr);
    struct tm ti;
    localtime_r(&now, &ti);
    snprintf(hhmm, sizeof(hhmm), "%02d:%02d", ti.tm_hour, ti.tm_min);
  }

  full_paint.Clear(UNCOLORED);

  // Bold-ish header
  char header[64];
  snprintf(header, sizeof(header), "HOLD | %s | PAGE %d/%d | TOTAL:%d",
           hhmm, sCurrentPage, sTotalPages, sTotalActive);
  full_paint.DrawStringAt(0, 5, header, &Font16, COLORED);
  full_paint.DrawStringAt(1, 5, header, &Font16, COLORED);

  int y = kLineH;
  const int maxShown = 5;
  const int start    = (sCurrentPage - 1) * maxShown;

  int shown = 0;
  for (int i = start; i < active && shown < maxShown; i++) {
    const auto& r = rows[i];

    // Callsign to display (fallback to ICAO)
    char callsign[9];
    if (r.callsign[0] == '\0') { strncpy(callsign, r.icao24, sizeof(callsign)); callsign[sizeof(callsign)-1] = '\0'; }
    else                       { strncpy(callsign, r.callsign, sizeof(callsign)); callsign[sizeof(callsign)-1] = '\0'; }

    char csPadded[8];  snprintf(csPadded, sizeof(csPadded), "%-7.7s", callsign);

    float dist = r.distance; if (dist > 99999.9f) dist = 99999.9f;
    char distPadded[12]; snprintf(distPadded, sizeof(distPadded), "%7.1f", dist);

    int   bInt = (int)r.bearing; if (bInt < 0) bInt += 360; if (bInt > 359) bInt -= 360;
    char  brg[4]; snprintf(brg, sizeof(brg), "%3d", bInt);

    char cd[3]; compass2(r.bearing, cd);

    char infoLine[96];
    snprintf(infoLine, sizeof(infoLine), "%s%skm %s%s %s",
             csPadded, distPadded, brg, cd, r.country);

    bool dark = (shown % 2 == 0);

    // Model line
    if (dark) {
      full_paint.DrawFilledRectangle(0, y, kScreenW-1, y + kLineH - 1, COLORED);
      full_paint.DrawStringAt(5, y + 5, r.model, &Font16, UNCOLORED);
    } else {
      full_paint.DrawStringAt(5, y + 5, r.model, &Font16, COLORED);
    }
    y += kLineH;

    // Info line
    if (dark) {
      full_paint.DrawFilledRectangle(0, y, kScreenW-1, y + kLineH - 1, COLORED);
      full_paint.DrawStringAt(5, y + 5, infoLine, &Font16, UNCOLORED);
    } else {
      full_paint.DrawStringAt(5, y + 5, infoLine, &Font16, COLORED);
    }
    y += kLineH;

    shown++;
  }

  // One FULL refresh
  epd.Init_Fast(0);
  epd.Display(full_paint.GetImage());
}

// -------- LIVE (batched partial, single refresh) --------
void drawAircraftInfoToDisplay_Partial(const char* timeStr, int /*totalAircraftFromAPI*/) {
  if (sMode == HOLD_MODE) return;

  RowView rows[MAX_CACHE_SIZE];
  int active = snapshotActiveRows(rows, MAX_CACHE_SIZE);
  std::sort(rows, rows + active, [](const RowView& a, const RowView& b){
    return a.distance < b.distance;
  });
  recalcPagingFromActive(active);

  if (timeStr && timeStr[0]) {
    strncpy(sLastTimeStr, timeStr, sizeof(sLastTimeStr) - 1);
    sLastTimeStr[sizeof(sLastTimeStr) - 1] = '\0';
  }

  ScopedDispLock _;

  const int maxShown = 5;
  int currentLine = 0, linesUsed = 0;
  const int maxLines = 1 + (maxShown * 2);

  // Header (write only)
  char header[64];
  snprintf(header, sizeof(header), "%s | TOTAL:%d",
           (timeStr && timeStr[0]) ? timeStr : sLastTimeStr, sTotalActive);
  paint.Clear(UNCOLORED);
  paint.DrawStringAt(0, 5, header, &Font16, COLORED);
  paint.DrawStringAt(1, 5, header, &Font16, COLORED);
  epd.Display_Partial_Not_refresh(paint.GetImage(), 0, currentLine, kScreenW, currentLine + kLineH);
  currentLine += kLineH; linesUsed++;

  int shown = 0;
  for (int i = 0; i < active && shown < maxShown; i++) {
    const auto& r = rows[i];

    char callsign[9];
    if (r.callsign[0] == '\0') { strncpy(callsign, r.icao24, sizeof(callsign)); callsign[sizeof(callsign)-1] = '\0'; }
    else                       { strncpy(callsign, r.callsign, sizeof(callsign)); callsign[sizeof(callsign)-1] = '\0'; }

    char csPadded[8];  snprintf(csPadded, sizeof(csPadded), "%-7.7s", callsign);

    float dist = r.distance; if (dist > 99999.9f) dist = 99999.9f;
    char distPadded[12]; snprintf(distPadded, sizeof(distPadded), "%7.1f", dist);

    int   bInt = (int)r.bearing; if (bInt < 0) bInt += 360; if (bInt > 359) bInt -= 360;
    char  brg[4]; snprintf(brg, sizeof(brg), "%3d", bInt);

    char cd[3]; compass2(r.bearing, cd);

    char infoLine[96];
    snprintf(infoLine, sizeof(infoLine), "%s%skm %s%s %s",
             csPadded, distPadded, brg, cd, r.country);

    bool dark = (shown % 2 == 0);

    paint.Clear(dark ? COLORED : UNCOLORED);
    paint.DrawStringAt(5, 5, r.model, &Font16, dark ? UNCOLORED : COLORED);
    epd.Display_Partial_Not_refresh(paint.GetImage(), 0, currentLine, kScreenW, currentLine + kLineH);
    currentLine += kLineH; linesUsed++;

    paint.Clear(dark ? COLORED : UNCOLORED);
    paint.DrawStringAt(5, 5, infoLine, &Font16, dark ? UNCOLORED : COLORED);
    epd.Display_Partial_Not_refresh(paint.GetImage(), 0, currentLine, kScreenW, currentLine + kLineH);
    currentLine += kLineH; linesUsed++;

    shown++;

    yield();
    if (gHoldRequested) return;
  }

  // Clear leftovers — write only
  for (int i = linesUsed; i < maxLines; i++) {
    paint.Clear(UNCOLORED);
    epd.Display_Partial_Not_refresh(paint.GetImage(), 0, i * kLineH, kScreenW, (i + 1) * kLineH);
    yield();
    if (gHoldRequested) return;
  }

  // Single partial refresh at the end
  if (!gHoldRequested) {
    epd.TurnOnDisplay_Partial();
  }
}

// -------- Mode & paging API --------
DisplayMode getDisplayMode() { return sMode; }

void setDisplayMode(DisplayMode m) {
  if (m == HOLD_MODE && getActiveCount() < 5) {
    return;
  }
  if (sMode == m) return;
  sMode = m;

  if (sMode == HOLD_MODE) {
    sCurrentPage = 1;          // no drawing here
    return;
  }

  // We’re going to LIVE mode:
  // 1) Do any full-panel init/clear while holding the mutex
  {
    ScopedDispLock _;
    epd.Init_Fast(0);
    epd.Clear();
  }

  // 2) Make sure the preempt flag isn’t set so the partial can complete
  gHoldRequested = false;

  // 3) Now draw the LIVE view (this function will take the mutex itself)
  drawAircraftInfoToDisplay_Partial(sLastTimeStr, 0);
}

void pageUp() {
  if (sMode != HOLD_MODE) return;
  recalcPagingFromActive(sTotalActive);
  if (sCurrentPage < sTotalPages) sCurrentPage++;
  drawHoldPageFullBuffer();         // one FULL refresh
}

void pageDown() {
  if (sMode != HOLD_MODE) return;
  recalcPagingFromActive(sTotalActive);
  if (sCurrentPage > 1) sCurrentPage--;
  drawHoldPageFullBuffer();         // one FULL refresh
}

void redrawHoldPageFull() {
  if (sMode != HOLD_MODE) return;
  drawHoldPageFullBuffer();
}

