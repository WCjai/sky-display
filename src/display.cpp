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
static int sHoldFlipCount = 0;
static const int kFullEvery = 8; // do a full refresh every 8 flips to kill ghosting

static constexpr int kScreenW = 400;
static constexpr int kScreenH = 300;
static constexpr int kLineH   = 27;
static bool sHoldPartialInit = false;

static bool sPanelPrimed = false;       // we have a valid base loaded
static bool sFastLUT     = false;       // panel is in fast/partial init

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

// ----- Split-flap helpers -----
static constexpr int FLAP_CELL_W = 12;             // box width per char
static constexpr int FLAP_CELL_H = kLineH;         // one text band high
static constexpr int FLAP_COLS   = kScreenW / FLAP_CELL_W; // 400/12 = 33 cols

// Draw one full-width "split-flap" line made of boxes, each holding 1 char.
// - text     : C-string to render (truncated/padded to FLAP_COLS)
// - invert   : if true, fills the *inside* of each cell black and draws white text
// - withBox  : if false, just draws text on white (keeps the centering math)
static void DrawFlapLine(Paint& p, const char* text, bool invert, bool withBox) {
  p.Clear(UNCOLORED);  // build the whole row in RAM once

  for (int col = 0; col < FLAP_COLS; ++col) {
    const char ch = (text && text[col]) ? text[col] : ' ';
    const int  x0 = col * FLAP_CELL_W;
    const int  x1 = x0 + FLAP_CELL_W - 1;

    if (withBox) {
      // 1px rectangle border
      p.DrawFilledRectangle(x0, 0,  x1, 0,  COLORED);            // top
      p.DrawFilledRectangle(x0, FLAP_CELL_H-1, x1, FLAP_CELL_H-1, COLORED);  // bottom
      p.DrawFilledRectangle(x0, 0,  x0, FLAP_CELL_H-1, COLORED); // left
      p.DrawFilledRectangle(x1, 0,  x1, FLAP_CELL_H-1, COLORED); // right
    }

    if (invert) {
      // fill the inside (leaving 1px border) so text is white-on-black
      if (withBox) {
        p.DrawFilledRectangle(x0+1, 1, x1-1, FLAP_CELL_H-2, COLORED);
      }
    }

    // Center the glyph inside the cell
    const int gx = x0 + (FLAP_CELL_W - Font16.Width)  / 2;
    const int gy =      (FLAP_CELL_H - Font16.Height) / 2;
    char buf[2] = { ch, 0 };
    p.DrawStringAt(gx, gy, buf, &Font16, invert ? UNCOLORED : COLORED);
  }
}


// ---- Split-flap, variable-length run (no full-row grid) ----
static void DrawFlapRun(Paint& p,
                        const char* text,
                        int left_margin_px = 5,   // where to start drawing
                        int box_gap_px     = 1,   // space between boxes
                        bool fillBlack     = false) // false = white bg + border
{
  // Render into a single band buffer (height = kLineH)
  p.Clear(UNCOLORED);

  if (!text) text = "";

  const int boxW = Font16.Width + 2;        // 1px padding left/right
  const int boxH = kLineH - 2;              // leave 1px band margin
  const int glyphY = (kLineH - Font16.Height) / 2;  // vertical center

  // Max chars that fit from left_margin to right edge
  int maxChars = 0;
  {
    int x = left_margin_px;
    while (x + boxW <= kScreenW) { maxChars++; x += boxW + box_gap_px; }
  }

  // How many chars to draw
  int n = (int)strlen(text);
  if (n > maxChars) n = maxChars;

  int x = left_margin_px;
  for (int i = 0; i < n; ++i) {
    const int x0 = x;
    const int y0 = 1;                 // 1px top margin inside band
    const int x1 = x0 + boxW - 1;
    const int y1 = y0 + boxH - 1;

    // Optional fill for inverted style
    if (fillBlack) p.DrawFilledRectangle(x0, y0, x1, y1, COLORED);

    // 1-px rectangle border
    p.DrawFilledRectangle(x0, y0, x1, y0, COLORED);   // top
    p.DrawFilledRectangle(x0, y1, x1, y1, COLORED);   // bottom
    p.DrawFilledRectangle(x0, y0, x0, y1, COLORED);   // left
    p.DrawFilledRectangle(x1, y0, x1, y1, COLORED);   // right

    // Center glyph in the box
    char ch[2] = { text[i], 0 };
    const int gx = x0 + (boxW - Font16.Width) / 2;
    p.DrawStringAt(gx, glyphY, ch, &Font16, fillBlack ? UNCOLORED : COLORED);

    x += boxW + box_gap_px;
  }
  // Note: the band outside the boxes is already white due to Clear().
}


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

void ensurePartialPrimed() {
  if (!sPanelPrimed) {
    epd.Init_Fast(Seconds_1S);          // fast LUT for partial updates
    // Use whatever is currently on screen as “base”: we can’t read it back,
    // so we push a white base ONCE. After this, stay in partial.
    full_paint.Clear(UNCOLORED);
    epd.Display_Base(full_paint.GetImage());
    sPanelPrimed = true;
    sFastLUT     = true;
  } else if (!sFastLUT) {
    // We were in normal LUT — switch to fast without disturbing base
    epd.Init_Fast(Seconds_1S);
    sFastLUT = true;
  }
}

static void holdMaybeFullRefresh() {
  if (++sHoldFlipCount >= kFullEvery) {
    sHoldFlipCount = 0;
    // Keep the same fast LUT; issue a partial "turn-on" to reduce ghosting.
    // This re-drives the currently written partial bands without clearing.
    epd.Init_Fast(Seconds_1S);
    epd.TurnOnDisplay_Partial();
  }
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

static inline void FillChecker(Paint& p, int w, int h) {
  // 50% checkerboard: write only half the pixels black
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      if (((x ^ y) & 1) == 0) {
        p.DrawAbsolutePixel(x, y, COLORED);   // black pixel
      }
    }
  }
}

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
// void drawAircraftInfoToDisplay_Partial(const char* timeStr, int /*totalAircraftFromAPI*/) {
//   if (sMode == HOLD_MODE) return;

//   RowView rows[MAX_CACHE_SIZE];
//   int active = snapshotActiveRows(rows, MAX_CACHE_SIZE);
//   std::sort(rows, rows + active, [](const RowView& a, const RowView& b){
//     return a.distance < b.distance;
//   });
//   recalcPagingFromActive(active);

//   if (timeStr && timeStr[0]) {
//     strncpy(sLastTimeStr, timeStr, sizeof(sLastTimeStr) - 1);
//     sLastTimeStr[sizeof(sLastTimeStr) - 1] = '\0';
//   }

//   ScopedDispLock _;

//   const int maxShown = 5;
//   int currentLine = 0, linesUsed = 0;
//   const int maxLines = 1 + (maxShown * 2);

//   // Header (write only)
//   char header[64];
//   snprintf(header, sizeof(header), "%s | TOTAL:%d",
//            (timeStr && timeStr[0]) ? timeStr : sLastTimeStr, sTotalActive);
//   paint.Clear(UNCOLORED);
//   paint.DrawStringAt(0, 5, header, &Font16, COLORED);
//   paint.DrawStringAt(1, 5, header, &Font16, COLORED);
//   epd.Display_Partial_Not_refresh(paint.GetImage(), 0, currentLine, kScreenW, currentLine + kLineH);
//   currentLine += kLineH; linesUsed++;

//   int shown = 0;
//   for (int i = 0; i < active && shown < maxShown; i++) {
//     const auto& r = rows[i];

//     char callsign[9];
//     if (r.callsign[0] == '\0') { strncpy(callsign, r.icao24, sizeof(callsign)); callsign[sizeof(callsign)-1] = '\0'; }
//     else                       { strncpy(callsign, r.callsign, sizeof(callsign)); callsign[sizeof(callsign)-1] = '\0'; }

//     char csPadded[8];  snprintf(csPadded, sizeof(csPadded), "%-7.7s", callsign);

//     float dist = r.distance; if (dist > 99999.9f) dist = 99999.9f;
//     char distPadded[12]; snprintf(distPadded, sizeof(distPadded), "%7.1f", dist);

//     int   bInt = (int)r.bearing; if (bInt < 0) bInt += 360; if (bInt > 359) bInt -= 360;
//     char  brg[4]; snprintf(brg, sizeof(brg), "%3d", bInt);

//     char cd[3]; compass2(r.bearing, cd);

//     char infoLine[96];
//     snprintf(infoLine, sizeof(infoLine), "%s%skm %s%s %s",
//              csPadded, distPadded, brg, cd, r.country);

//     bool dark = (shown % 2 == 0);

//     paint.Clear(dark ? COLORED : UNCOLORED);
//     if (dark) FillChecker(paint, kScreenW, kLineH);  // dither instead of solid fill
//     paint.DrawStringAt(5, 5, r.model, &Font16, dark ? UNCOLORED : COLORED);
//     epd.Display_Partial_Not_refresh(paint.GetImage(), 0, currentLine, kScreenW, currentLine + kLineH);
//     currentLine += kLineH; linesUsed++;

//     paint.Clear(dark ? COLORED : UNCOLORED);
//     if (dark) FillChecker(paint, kScreenW, kLineH);  // dither instead of solid fill
//     paint.DrawStringAt(5, 5, infoLine, &Font16, dark ? UNCOLORED : COLORED);
//     epd.Display_Partial_Not_refresh(paint.GetImage(), 0, currentLine, kScreenW, currentLine + kLineH);
//     currentLine += kLineH; linesUsed++;

//     shown++;

//     yield();
//     if (gHoldRequested) return;
//   }

//   // Clear leftovers — write only
//   for (int i = linesUsed; i < maxLines; i++) {
//     paint.Clear(UNCOLORED);
//     epd.Display_Partial_Not_refresh(paint.GetImage(), 0, i * kLineH, kScreenW, (i + 1) * kLineH);
//     yield();
//     if (gHoldRequested) return;
//   }

//   // Single partial refresh at the end
//   if (!gHoldRequested) {
//     epd.TurnOnDisplay_Partial();
//   }
// }

// whiteback ground
// void drawAircraftInfoToDisplay_Partial(const char* timeStr, int /*totalAircraftFromAPI*/) {
//   if (sMode == HOLD_MODE) return;

//   RowView rows[MAX_CACHE_SIZE];
//   int active = snapshotActiveRows(rows, MAX_CACHE_SIZE);
//   std::sort(rows, rows + active, [](const RowView& a, const RowView& b){
//     return a.distance < b.distance;
//   });
//   recalcPagingFromActive(active);

//   if (timeStr && timeStr[0]) {
//     strncpy(sLastTimeStr, timeStr, sizeof(sLastTimeStr) - 1);
//     sLastTimeStr[sizeof(sLastTimeStr) - 1] = '\0';
//   }

//   ScopedDispLock _;

//   const int maxShown = 5;
//   int y = 0;
//   int linesUsed = 0;
//   const int maxLines = 1 + (maxShown * 2);

//   // ---- Header (white bg, black text) ----
//   char header[64];
//   snprintf(header, sizeof(header), "%s | TOTAL:%d",
//            (timeStr && timeStr[0]) ? timeStr : sLastTimeStr, sTotalActive);
//   paint.Clear(UNCOLORED);
//   paint.DrawStringAt(0, 5, header, &Font16, COLORED);
//   paint.DrawStringAt(1, 5, header, &Font16, COLORED); // fake bold
//   epd.Display_Partial_Not_refresh(paint.GetImage(), 0, y, kScreenW, y + kLineH);
//   y += kLineH; linesUsed++;

//   // ---- Rows: one bordered "card" = 2 bands (model + info) ----
//   int shown = 0;
//   for (int i = 0; i < active && shown < maxShown; i++) {
//     const auto& r = rows[i];

//     // Callsign (fallback to ICAO)
//     char callsign[9];
//     if (r.callsign[0] == '\0') { strncpy(callsign, r.icao24, sizeof(callsign)); callsign[sizeof(callsign)-1] = '\0'; }
//     else                       { strncpy(callsign, r.callsign, sizeof(callsign)); callsign[sizeof(callsign)-1] = '\0'; }
//     char csPadded[8];  snprintf(csPadded, sizeof(csPadded), "%-7.7s", callsign);

//     float dist = r.distance; if (dist > 99999.9f) dist = 99999.9f;
//     char  distPadded[12]; snprintf(distPadded, sizeof(distPadded), "%7.1f", dist);

//     int   bInt = (int)r.bearing; if (bInt < 0) bInt += 360; if (bInt > 359) bInt -= 360;
//     char  brg[4]; snprintf(brg, sizeof(brg), "%3d", bInt);

//     char cd[3]; compass2(r.bearing, cd);

//     char infoLine[96];
//     snprintf(infoLine, sizeof(infoLine), "%s%skm %s%s %s",
//              csPadded, distPadded, brg, cd, r.country);

//     const int yCardTop = y;

//     // --- Model band (white) ---
//     paint.Clear(UNCOLORED);
//     // Top + left/right border for the card
//     paint.DrawFilledRectangle(0, 0, kScreenW-1, 0, COLORED);           // top 1px
//     paint.DrawFilledRectangle(0, 0, 0, kLineH-1, COLORED);             // left 1px
//     paint.DrawFilledRectangle(kScreenW-1, 0, kScreenW-1, kLineH-1, COLORED); // right 1px
//     paint.DrawStringAt(5, 5, r.model, &Font16, COLORED);
//     epd.Display_Partial_Not_refresh(paint.GetImage(), 0, y, kScreenW, y + kLineH);
//     y += kLineH; linesUsed++;

//     // --- Info band (white) ---
//     paint.Clear(UNCOLORED);
//     // Bottom + left/right border for the card
//     paint.DrawFilledRectangle(0, kLineH-1, kScreenW-1, kLineH-1, COLORED);    // bottom 1px
//     paint.DrawFilledRectangle(0, 0, 0, kLineH-1, COLORED);                    // left 1px
//     paint.DrawFilledRectangle(kScreenW-1, 0, kScreenW-1, kLineH-1, COLORED);  // right 1px
//     paint.DrawStringAt(5, 5, infoLine, &Font16, COLORED);
//     epd.Display_Partial_Not_refresh(paint.GetImage(), 0, y, kScreenW, y + kLineH);
//     y += kLineH; linesUsed++;

//     (void)yCardTop; // kept in case you later add shading inside the card
//     shown++;

//     yield();
//     if (gHoldRequested) return;
//   }

//   // ---- Clear leftovers (white) ----
//   for (int i = linesUsed; i < maxLines; i++) {
//     paint.Clear(UNCOLORED);
//     epd.Display_Partial_Not_refresh(paint.GetImage(), 0, i * kLineH, kScreenW, (i + 1) * kLineH);
//     yield();
//     if (gHoldRequested) return;
//   }

//   // Present once
//   if (!gHoldRequested) {
//     epd.TurnOnDisplay_Partial();
//   }
// }

// ---- Header drawing helpers ----
static inline void DrawVLine(Paint& p, int x, int y1, int y2, int thickness, int color) {
  if (y2 < y1) std::swap(y1, y2);
  for (int t = 0; t < thickness; ++t) {
    p.DrawFilledRectangle(x + t, y1, x + t, y2, color);
  }
}

static inline void DrawHLine(Paint& p, int x1, int x2, int y, int thickness, int color) {
  if (x2 < x1) std::swap(x1, x2);
  for (int t = 0; t < thickness; ++t) {
    p.DrawFilledRectangle(x1, y + t, x2, y + t, color);
  }
}

// simple rounded-rect outline (radius 4..6 looks nice at 27px band height)
static void DrawRoundedRectOutline(Paint& p, int x, int y, int w, int h, int r, int thickness, int color) {
  if (r < 1) r = 1;
  if (r*2 > w) r = w/2;
  if (r*2 > h) r = h/2;

  // straight edges
  DrawHLine(p, x + r, x + w - r - 1, y,             thickness, color); // top
  DrawHLine(p, x + r, x + w - r - 1, y + h - 1 - thickness + 1, thickness, color); // bottom
  DrawVLine(p, x,             y + r, y + h - r - 1, thickness, color); // left
  DrawVLine(p, x + w - thickness, y + r, y + h - r - 1, thickness, color); // right

  // crude quarter arcs (symmetric pixels) – good enough at this scale
  for (int i = 0; i < r; ++i) {
    int dx = r - i;
    int dy = (int)roundf(sqrtf((float)r * r - dx * dx));
    // top-left
    p.DrawAbsolutePixel(x + r - dx, y + r - dy, color);
    // top-right
    p.DrawAbsolutePixel(x + w - r - 1 + dx - 1, y + r - dy, color);
    // bottom-left
    p.DrawAbsolutePixel(x + r - dx, y + h - r + dy - 1, color);
    // bottom-right
    p.DrawAbsolutePixel(x + w - r - 1 + dx - 1, y + h - r + dy - 1, color);
  }

  // thickness > 1: inflate inward
  for (int t = 1; t < thickness; ++t) {
    DrawRoundedRectOutline(p, x + t, y + t, w - 2*t, h - 2*t, r - (t>0?1:0), 1, color);
  }
}

// Parse "YYYY-MM-DD | HH:MM:SS" or "YYYY-MM-DD | hh:mm:ss AM/PM"
static void ParseHeaderParts(const char* src,
                             char outY[5], char outM[3], char outD[3],
                             char outTime[9], bool& hasAMPM, char outAMPM[3]) {
  outY[0]=outM[0]=outD[0]=outTime[0]=outAMPM[0]='\0';
  hasAMPM = false;
  if (!src || !src[0]) return;

  // Expect at least "YYYY-MM-DD"
  if (strlen(src) >= 10 && isdigit((unsigned char)src[0])) {
    memcpy(outY, src+0, 4); outY[4]='\0';
    memcpy(outM, src+5, 2); outM[2]='\0';
    memcpy(outD, src+8, 2); outD[2]='\0';
  }

  // find first '|'
  const char* bar = strchr(src, '|');
  if (bar) {
    // move past '|', skip spaces
    bar++;
    while (*bar==' '){ ++bar; }
    // copy HH:MM:SS (8 chars) if possible
    if (strlen(bar) >= 8 && isdigit((unsigned char)bar[0])) {
      memcpy(outTime, bar, 8);
      outTime[8]='\0';
      // check for AM/PM after a space
      const char* sp = bar + 8;
      while (*sp==' ') ++sp;
      if ((sp[0]=='A' || sp[0]=='P') && (sp[1]=='M')) {
        outAMPM[0]=sp[0]; outAMPM[1]='M'; outAMPM[2]='\0';
        hasAMPM = true;
      }
    }
  }
}

//dotted box border
// void drawAircraftInfoToDisplay_Partial(const char* timeStr, int /*totalAircraftFromAPI*/) {
//   if (sMode == HOLD_MODE) return;

//   RowView rows[MAX_CACHE_SIZE];
//   int active = snapshotActiveRows(rows, MAX_CACHE_SIZE);
//   std::sort(rows, rows + active, [](const RowView& a, const RowView& b){
//     return a.distance < b.distance;
//   });
//   recalcPagingFromActive(active);

//   if (timeStr && timeStr[0]) {
//     strncpy(sLastTimeStr, timeStr, sizeof(sLastTimeStr) - 1);
//     sLastTimeStr[sizeof(sLastTimeStr) - 1] = '\0';
//   }

//   ScopedDispLock _;

//   const int maxShown = 5;
//   const int maxLines = 1 + (maxShown * 2);

//   // font metrics
//   const int fw = Font16.Width;
//   const int fh = Font16.Height;

//   // line cosmetics
//   const int kSepThickness = 1; // solid borders
//   const int kDotThickness = 1; // dotted lines
//   const int kDotOn  = 8;       // dot length
//   const int kDotOff = 6;       // gap length

//   auto drawSolidHLine = [&](int y0, int thickness) {
//     for (int t = 0; t < thickness; ++t) {
//       paint.DrawFilledRectangle(0, y0 + t, kScreenW - 1, y0 + t, COLORED);
//     }
//   };
//   auto drawDottedHLine = [&](int y0, int thickness, int onLen, int offLen) {
//     for (int t = 0; t < thickness; ++t) {
//       for (int x = 0; x < kScreenW; x += (onLen + offLen)) {
//         int x1 = x;
//         int x2 = x + onLen - 1;
//         if (x1 >= kScreenW) break;
//         if (x2 >= kScreenW) x2 = kScreenW - 1;
//         paint.DrawFilledRectangle(x1, y0 + t, x2, y0 + t, COLORED);
//       }
//     }
//   };
//   auto drawDottedVLine = [&](int x0, int thickness, int onLen, int offLen) {
//     for (int t = 0; t < thickness; ++t) {
//       for (int y = 0; y < kLineH; y += (onLen + offLen)) {
//         int y1 = y;
//         int y2 = y + onLen - 1;
//         if (y1 >= kLineH) break;
//         if (y2 >= kLineH) y2 = kLineH - 1;
//         paint.DrawFilledRectangle(x0 + t, y1, x0 + t, y2, COLORED);
//       }
//     }
//   };

//   int y = 0;
//   int linesUsed = 0;

//   // ---- Header ----
//   char header[64];
//   snprintf(header, sizeof(header), "%s | TOTAL:%d",
//            (timeStr && timeStr[0]) ? timeStr : sLastTimeStr, sTotalActive);
//   paint.Clear(UNCOLORED);
//   paint.DrawStringAt(0, 5, header, &Font16, COLORED);
//   paint.DrawStringAt(1, 5, header, &Font16, COLORED); // fake bold
//   epd.Display_Partial_Not_refresh(paint.GetImage(), 0, y, kScreenW, y + kLineH);
//   y += kLineH; linesUsed++;

//   // ---- Rows ----
//   int shown = 0;
//   for (int i = 0; i < active && shown < maxShown; i++) {
//     const auto& r = rows[i];

//     // Callsign (fallback to ICAO) — fixed 7 chars
//     char callsign[9];
//     if (r.callsign[0] == '\0') { strncpy(callsign, r.icao24, sizeof(callsign)); callsign[sizeof(callsign)-1] = '\0'; }
//     else                       { strncpy(callsign, r.callsign, sizeof(callsign)); callsign[sizeof(callsign)-1] = '\0'; }
//     char csPadded[8]; snprintf(csPadded, sizeof(csPadded), "%-7.7s", callsign);

//     // Distance — FIXED WIDTH "xx.xkm" (or " 9.9km"), always 6 chars
//     float dist = r.distance; if (dist > 99.9f) dist = 99.9f;
//     char distStr[8]; snprintf(distStr, sizeof(distStr), "%4.1fkm", dist); // 4.1f => "99.9" or " 9.9" → + "km" = 6 chars

//     // Direction — "dddXY" (e.g., "288W ")
//     int  bInt = (int)r.bearing; if (bInt < 0) bInt += 360; if (bInt > 359) bInt -= 360;
//     char brg[4];  snprintf(brg, sizeof(brg), "%3d", bInt);
//     char cd[3];   compass2(r.bearing, cd);
//     char dirStr[8]; snprintf(dirStr, sizeof(dirStr), "%s%s", brg, cd); // ~5 chars

//     // --- Model band ---
//     paint.Clear(UNCOLORED);
//     drawSolidHLine(0, kSepThickness);                         // top border
//     paint.DrawStringAt(5, 5, r.model, &Font16, COLORED);
//     drawDottedHLine(kLineH - 1, kDotThickness, kDotOn, kDotOff); // dotted under model
//     epd.Display_Partial_Not_refresh(paint.GetImage(), 0, y, kScreenW, y + kLineH);
//     y += kLineH; linesUsed++;

//     // --- Info band with tighter spacing + dotted verticals ---
//     paint.Clear(UNCOLORED);

//     // const int left = 5;
//     // const int pad  = 4;     // space before dotted line
//     // const int spaceAfterDot = fw; // +1 space after dotted line

//     // // Column widths (in characters) for uniform layout
//     // const int col1ch = 7;   // callsign
//     // const int col2ch = 7;   // distance "xx.xkm" CHNAGE HEEER
//     // const int col3ch = 5;   // direction "dddXY"

//     // // Compute x positions
//     // int x1_text = left;
//     // int x1_line = x1_text + col1ch * fw + pad;
//     // int x2_text = x1_line + spaceAfterDot; // +1 space after dotted line
//     // int x2_line = x2_text + col2ch * fw + pad;
//     // int x3_text = x2_line + spaceAfterDot; // +1 space after dotted line
//     // int x3_line = x3_text + col3ch * fw + pad;
//     // int x4_text = x3_line + spaceAfterDot; // +1 space after dotted line

//     // // Country fits the remaining width
//     // char countryBuf[32];
//     // strncpy(countryBuf, r.country, sizeof(countryBuf)-1);
//     // countryBuf[sizeof(countryBuf)-1] = '\0';
//     // int maxCountryPx = kScreenW - 5 - x4_text;
//     // int maxCountryCh = (maxCountryPx > 0) ? (maxCountryPx / fw) : 0;
//     // if ((int)strlen(countryBuf) > maxCountryCh) countryBuf[maxCountryCh] = '\0';
//     const int left = 5;
//     const int pad  = 4;     // space before dotted line
//     const int spaceAfterDot = fw; // +1 space after dotted line

//     // Column widths (in characters) for uniform layout
//     const int col1ch = 7;   // callsign
//     const int col2ch = 7;   // distance "xxx.xkm" (now supports 999.9 km)
//     const int col3ch = 5;   // direction "dddXY"

//     // Compute x positions
//     int x1_text = left;
//     int x1_line = x1_text + col1ch * fw + pad;
//     int x2_text = x1_line + spaceAfterDot; // +1 space after dotted line
//     int x2_line = x2_text + col2ch * fw + pad;
//     int x3_text = x2_line + spaceAfterDot; // +1 space after dotted line
//     int x3_line = x3_text + col3ch * fw + pad;
//     int x4_text = x3_line + spaceAfterDot; // +1 space after dotted line

//     // Country fits the remaining width
//     char countryBuf[32];
//     strncpy(countryBuf, r.country, sizeof(countryBuf)-1);
//     countryBuf[sizeof(countryBuf)-1] = '\0';
//     int maxCountryPx = kScreenW - 5 - x4_text;
//     int maxCountryCh = (maxCountryPx > 0) ? (maxCountryPx / fw) : 0;
//     if ((int)strlen(countryBuf) > maxCountryCh) countryBuf[maxCountryCh] = '\0';

//     // Draw texts
//     paint.DrawStringAt(x1_text, 5, csPadded,   &Font16, COLORED);
//     paint.DrawStringAt(x2_text, 5, distStr,    &Font16, COLORED);
//     paint.DrawStringAt(x3_text, 5, dirStr,     &Font16, COLORED);
//     paint.DrawStringAt(x4_text, 5, countryBuf, &Font16, COLORED);

//     // Vertical dotted separators
//     if (x1_line < kScreenW) drawDottedVLine(x1_line, kDotThickness, kDotOn, kDotOff);
//     if (x2_line < kScreenW) drawDottedVLine(x2_line, kDotThickness, kDotOn, kDotOff);
//     if (x3_line < kScreenW) drawDottedVLine(x3_line, kDotThickness, kDotOn, kDotOff);


//     // bottom border for the card
//     drawSolidHLine(kLineH - kSepThickness, kSepThickness);

//     epd.Display_Partial_Not_refresh(paint.GetImage(), 0, y, kScreenW, y + kLineH);
//     y += kLineH; linesUsed++;

//     shown++;
//     yield();
//     if (gHoldRequested) return;
//   }

//   // Clear leftovers
//   for (int i = linesUsed; i < maxLines; i++) {
//     paint.Clear(UNCOLORED);
//     epd.Display_Partial_Not_refresh(paint.GetImage(), 0, i * kLineH, kScreenW, (i + 1) * kLineH);
//     yield();
//     if (gHoldRequested) return;
//   }

//   if (!gHoldRequested) epd.TurnOnDisplay_Partial();
// }

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
  const int maxLines = 1 + (maxShown * 2);

  // font metrics
  const int fw = Font16.Width;
  const int fh = Font16.Height;

  // line cosmetics
  const int kSepThickness = 1; // solid borders
  const int kDotThickness = 1; // dotted lines
  const int kDotOn  = 4;       // dot length
  const int kDotOff = 3;       // gap length

  auto drawSolidHLine = [&](int y0, int thickness) {
    for (int t = 0; t < thickness; ++t) {
      paint.DrawFilledRectangle(0, y0 + t, kScreenW - 1, y0 + t, COLORED);
    }
  };
  auto drawDottedHLine = [&](int y0, int thickness, int onLen, int offLen) {
    for (int t = 0; t < thickness; ++t) {
      for (int x = 0; x < kScreenW; x += (onLen + offLen)) {
        int x1 = x;
        int x2 = x + onLen - 1;
        if (x1 >= kScreenW) break;
        if (x2 >= kScreenW) x2 = kScreenW - 1;
        paint.DrawFilledRectangle(x1, y0 + t, x2, y0 + t, COLORED);
      }
    }
  };
  auto drawDottedVLine = [&](int x0, int thickness, int onLen, int offLen) {
    for (int t = 0; t < thickness; ++t) {
      for (int y = 0; y < kLineH; y += (onLen + offLen)) {
        int y1 = y;
        int y2 = y + onLen - 1;
        if (y1 >= kLineH) break;
        if (y2 >= kLineH) y2 = kLineH - 1;
        paint.DrawFilledRectangle(x0 + t, y1, x0 + t, y2, COLORED);
      }
    }
  };

  int y = 0;
  int linesUsed = 0;
  // =========================
  // Stylized rounded HEADER  (date LEFT, time CENTER, total RIGHT)
  // =========================
  {
    const char* src = (timeStr && timeStr[0]) ? timeStr : sLastTimeStr;

    // Parse: YYYY-MM-DD | HH:MM:SS [AM/PM]
    char yy[5], mm[3], dd[3], hhmmss[9], ampm[3];
    bool haveAMPM = false;
    ParseHeaderParts(src, yy, mm, dd, hhmmss, haveAMPM, ampm);

    // Capsule metrics
    const int radius = 8;
    const int padX   = 6;    // inner horizontal padding in the header band
    const int yText  = 5;    // baseline Y for Font16
    const int topY   = 2;    // capsule inner top for vertical lines
    const int botY   = kLineH - 3;

    // Pre-calc widths
    const int fw = Font16.Width;
    int timeW = 8 * fw;                         // "HH:MM:SS"
    if (haveAMPM) timeW += fw + 2 * fw;         // +1 space + "AM"/"PM"

    char totalBuf[8]; snprintf(totalBuf, sizeof(totalBuf), "%d", sTotalActive);
    const char* totalLabel = "TOTAL";
    int totalLabelW = 5 * fw;                   // "TOTAL"
    int totalValW   = (int)strlen(totalBuf) * fw;
    // TOTAL | n  (thin line between label and value, plus 1 space after the line)
    int totalBlockW = totalLabelW + 0 /*line px*/ + fw /* 1 space */ + totalValW;

    // Clear and draw capsule (no bottom line)
    paint.Clear(UNCOLORED);
    DrawRoundedRectOutline(paint, 2, 1, kScreenW - 4, kLineH - 2, radius, 2, COLORED);

    // ---- Left: DATE (YYYY | MM | DD) ----
    int xL = padX + 4;  // a touch of extra left padding
    // YYYY
    paint.DrawStringAt(xL, yText, yy, &Font16, COLORED);
    // thin line after YYYY
    DrawVLine(paint, xL + 4 * fw + 1, topY + 2, botY - 2, 1, COLORED);
    // MM
    int xMM = xL + 4 * fw + 3; // +2px gap around the thin line
    paint.DrawStringAt(xMM, yText, mm, &Font16, COLORED);
    // thin line after MM
    DrawVLine(paint, xMM + 2 * fw + 1, topY + 2, botY - 2, 1, COLORED);
    // DD
    int xDD = xMM + 2 * fw + 3;
    paint.DrawStringAt(xDD, yText, dd, &Font16, COLORED);

    // ---- Center: TIME (HH:MM:SS [AM/PM]) ----
    int xC = (kScreenW - timeW) / 2;
    paint.DrawStringAt(xC, yText, hhmmss, &Font16, COLORED);
    if (haveAMPM) {
      // thin line just before AM/PM to visually split within the time block
      DrawVLine(paint, xC + 8 * fw + 1, topY + 2, botY - 2, 1, COLORED);
      paint.DrawStringAt(xC + 8 * fw + 3, yText, ampm, &Font16, COLORED);
    }

    // ---- Right: TOTAL | n (right aligned) ----
    int xR = kScreenW - padX - totalBlockW;
    // "TOTAL"
    paint.DrawStringAt(xR, yText, totalLabel, &Font16, COLORED);
    // thin line between TOTAL and value
    DrawVLine(paint, xR + totalLabelW + 1, topY + 2, botY - 2, 1, COLORED);
    // value with one space after the line
    paint.DrawStringAt(xR + totalLabelW + 3, yText, totalBuf, &Font16, COLORED);

    // Push header band (no extra underline!)
    epd.Display_Partial_Not_refresh(paint.GetImage(), 0, y, kScreenW, y + kLineH);
    y += kLineH; linesUsed++;
  }



  // ---- Rows ----
  int shown = 0;
  for (int i = 0; i < active && shown < maxShown; i++) {
    const auto& r = rows[i];

    // Callsign (fallback to ICAO) — fixed 7 chars
    char callsign[9];
    if (r.callsign[0] == '\0') { strncpy(callsign, r.icao24, sizeof(callsign)); callsign[sizeof(callsign)-1] = '\0'; }
    else                       { strncpy(callsign, r.callsign, sizeof(callsign)); callsign[sizeof(callsign)-1] = '\0'; }
    char csPadded[8]; snprintf(csPadded, sizeof(csPadded), "%-7.7s", callsign);

    // Distance — FIXED WIDTH "xx.xkm" (clamped to 99.9)
    float dist = r.distance; if (dist > 99.9f) dist = 99.9f;
    char distStr[8]; snprintf(distStr, sizeof(distStr), "%4.1fkm", dist);

    // Direction — "dddXY"
    int  bInt = (int)r.bearing; if (bInt < 0) bInt += 360; if (bInt > 359) bInt -= 360;
    char brg[4];  snprintf(brg, sizeof(brg), "%3d", bInt);
    char cd[3];   compass2(r.bearing, cd);
    char dirStr[8]; snprintf(dirStr, sizeof(dirStr), "%s%s", brg, cd);

    // --- Model band ---
    paint.Clear(UNCOLORED);

    // Remove the top border if this is the very first row after the header
    if (shown > 0) {  
        drawSolidHLine(0, kSepThickness); // top border
    }

    paint.DrawStringAt(5, 5, r.model, &Font16, COLORED);
    drawDottedHLine(kLineH - 1, kDotThickness, kDotOn, kDotOff); // dotted under model
    epd.Display_Partial_Not_refresh(paint.GetImage(), 0, y, kScreenW, y + kLineH);
    y += kLineH; linesUsed++;

    // --- Info band with dotted verticals ---
    paint.Clear(UNCOLORED);

    const int left = 5;
    const int pad  = 4;     // space before dotted line
    const int spaceAfterDot = fw; // +1 space after dotted line

    const int col1ch = 7;   // callsign
    const int col2ch = 7;   // distance "xxx.xkm" if you later extend to 999.9km
    const int col3ch = 5;   // direction "dddXY"

    int x1_text = left;
    int x1_line = x1_text + col1ch * fw + pad;
    int x2_text = x1_line + spaceAfterDot;
    int x2_line = x2_text + col2ch * fw + pad;
    int x3_text = x2_line + spaceAfterDot;
    int x3_line = x3_text + col3ch * fw + pad;
    int x4_text = x3_line + spaceAfterDot;

    // Country fits remaining width
    char countryBuf[32];
    strncpy(countryBuf, r.country, sizeof(countryBuf)-1);
    countryBuf[sizeof(countryBuf)-1] = '\0';
    int maxCountryPx = kScreenW - 5 - x4_text;
    int maxCountryCh = (maxCountryPx > 0) ? (maxCountryPx / fw) : 0;
    if ((int)strlen(countryBuf) > maxCountryCh) countryBuf[maxCountryCh] = '\0';

    paint.DrawStringAt(x1_text, 5, csPadded,   &Font16, COLORED);
    paint.DrawStringAt(x2_text, 5, distStr,    &Font16, COLORED);
    paint.DrawStringAt(x3_text, 5, dirStr,     &Font16, COLORED);
    paint.DrawStringAt(x4_text, 5, countryBuf, &Font16, COLORED);

    if (x1_line < kScreenW) drawDottedVLine(x1_line, kDotThickness, kDotOn, kDotOff);
    if (x2_line < kScreenW) drawDottedVLine(x2_line, kDotThickness, kDotOn, kDotOff);
    if (x3_line < kScreenW) drawDottedVLine(x3_line, kDotThickness, kDotOn, kDotOff);

    drawSolidHLine(kLineH - kSepThickness, kSepThickness); // bottom border
    epd.Display_Partial_Not_refresh(paint.GetImage(), 0, y, kScreenW, y + kLineH);
    y += kLineH; linesUsed++;

    shown++;
    yield();
    if (gHoldRequested) return;
  }

  // Clear leftovers
  for (int i = linesUsed; i < maxLines; i++) {
    paint.Clear(UNCOLORED);
    epd.Display_Partial_Not_refresh(paint.GetImage(), 0, i * kLineH, kScreenW, (i + 1) * kLineH);
    yield();
    if (gHoldRequested) return;
  }

  if (!gHoldRequested) epd.TurnOnDisplay_Partial();
}



// boxews
// void drawAircraftInfoToDisplay_Partial(const char* timeStr, int /*totalAircraftFromAPI*/) {
//   if (sMode == HOLD_MODE) return;

//   RowView rows[MAX_CACHE_SIZE];
//   int active = snapshotActiveRows(rows, MAX_CACHE_SIZE);
//   std::sort(rows, rows + active, [](const RowView& a, const RowView& b){
//     return a.distance < b.distance;
//   });
//   recalcPagingFromActive(active);

//   if (timeStr && timeStr[0]) {
//     strncpy(sLastTimeStr, timeStr, sizeof(sLastTimeStr) - 1);
//     sLastTimeStr[sizeof(sLastTimeStr) - 1] = '\0';
//   }

//   ScopedDispLock _;

//   const int maxShown = 5;
//   int y = 0;
//   int linesUsed = 0;
//   const int maxLines = 1 + (maxShown * 2);

//   // ---- Header ----
//   char header[64];
//   snprintf(header, sizeof(header), "%s | TOTAL:%d",
//            (timeStr && timeStr[0]) ? timeStr : sLastTimeStr, sTotalActive);
//   paint.Clear(UNCOLORED);
//   paint.DrawStringAt(0, 5, header, &Font16, COLORED);
//   paint.DrawStringAt(1, 5, header, &Font16, COLORED); // fake bold
//   epd.Display_Partial_Not_refresh(paint.GetImage(), 0, y, kScreenW, y + kLineH);
//   y += kLineH; linesUsed++;

//   // ---- Rows ----
//   int shown = 0;
//   for (int i = 0; i < active && shown < maxShown; i++) {
//     const auto& r = rows[i];

//     // Callsign (fallback to ICAO)
//     char callsign[9];
//     if (r.callsign[0] == '\0') { strncpy(callsign, r.icao24, sizeof(callsign)); callsign[sizeof(callsign)-1] = '\0'; }
//     else                       { strncpy(callsign, r.callsign, sizeof(callsign)); callsign[sizeof(callsign)-1] = '\0'; }
//     char csPadded[8];  snprintf(csPadded, sizeof(csPadded), "%-7.7s", callsign);

//     float dist = r.distance; if (dist > 99999.9f) dist = 99999.9f;
//     char  distPadded[12]; snprintf(distPadded, sizeof(distPadded), "%7.1f", dist);

//     int   bInt = (int)r.bearing; if (bInt < 0) bInt += 360; if (bInt > 359) bInt -= 360;
//     char  brg[4]; snprintf(brg, sizeof(brg), "%3d", bInt);

//     char cd[3]; compass2(r.bearing, cd);

//     char infoLine[96];
//     snprintf(infoLine, sizeof(infoLine), "%s%skm %s%s %s",
//              csPadded, distPadded, brg, cd, r.country);

//     // --- Model band (boxes, then overlay borders) ---
//     DrawFlapRun(paint, r.model, /*left*/5, /*gap*/1, /*fillBlack*/false);
//     // card top + left/right borders
//     paint.DrawFilledRectangle(0, 0, kScreenW-1, 1, COLORED);                 // top
//     //paint.DrawFilledRectangle(0, 0, 0, kLineH-1, COLORED);                   // left
//     //paint.DrawFilledRectangle(kScreenW-1, 0, kScreenW-1, kLineH-1, COLORED); // right
//     epd.Display_Partial_Not_refresh(paint.GetImage(), 0, y, kScreenW, y + kLineH);
//     y += kLineH; linesUsed++;

//     // --- Info band (boxes, then overlay borders) ---
//     DrawFlapRun(paint, infoLine, /*left*/5, /*gap*/1, /*fillBlack*/false);
//     // card bottom + left/right borders
//     paint.DrawFilledRectangle(0, kLineH-2, kScreenW-1, kLineH-1, COLORED);   // bottom
//     //paint.DrawFilledRectangle(0, 0, 0, kLineH-1, COLORED);                   // left
//     //paint.DrawFilledRectangle(kScreenW-1, 0, kScreenW-1, kLineH-1, COLORED); // right
//     epd.Display_Partial_Not_refresh(paint.GetImage(), 0, y, kScreenW, y + kLineH);
//     y += kLineH; linesUsed++;

//     shown++;

//     yield();
//     if (gHoldRequested) return;
//   }

//   // ---- Clear leftovers ----
//   for (int i = linesUsed; i < maxLines; i++) {
//     paint.Clear(UNCOLORED);
//     epd.Display_Partial_Not_refresh(paint.GetImage(), 0, i * kLineH, kScreenW, (i + 1) * kLineH);
//     yield();
//     if (gHoldRequested) return;
//   }

//   if (!gHoldRequested) epd.TurnOnDisplay_Partial();
// }


// -------- Mode & paging API --------
DisplayMode getDisplayMode() { return sMode; }


void setDisplayMode(DisplayMode m) {
  // Guard: don’t enter HOLD when <5 aircraft
  if (m == HOLD_MODE && getActiveCount() < 6) return;

  if (sMode == m) {
    // If pressing again in HOLD, redraw current page
    if (m == HOLD_MODE) {
      recalcPagingFromActive(getActiveCount());
      sCurrentPage = max(1, min(sCurrentPage, sTotalPages));
      ensurePartialPrimed();
      drawHoldPagePartial();
    }
    return;
  }

  // SWITCH
  sMode = m;

  if (sMode == HOLD_MODE) {
    // Stay in partial, do NOT clear or push a white base here
    ensurePartialPrimed();
    recalcPagingFromActive(getActiveCount());
    sCurrentPage = 1;
    drawHoldPagePartial();                // partial-only
  } else {
    // LIVE mode: partial-only as well (no Clear)
    ensurePartialPrimed();
    drawAircraftInfoToDisplay_Partial(sLastTimeStr, 0);
  }
}


void drawHoldPagePartial() {
  // Snapshot & sort
  RowView rows[MAX_CACHE_SIZE];
  int active = snapshotActiveRows(rows, MAX_CACHE_SIZE);
  std::sort(rows, rows + active, [](const RowView& a, const RowView& b){
    return a.distance < b.distance;
  });
  recalcPagingFromActive(active);

  const int maxShown = 5;
  const int start    = (sCurrentPage - 1) * maxShown;

  // ===== helpers (match LIVE look) =====
  const int fw = Font16.Width;
  const int kSepThickness = 1; // solid borders
  const int kDotThickness = 1; // dotted lines
  const int kDotOn  = 4;       // dot length
  const int kDotOff = 3;       // gap length

  auto drawSolidHLine = [&](int y0, int thickness) {
    for (int t = 0; t < thickness; ++t) {
      paint.DrawFilledRectangle(0, y0 + t, kScreenW - 1, y0 + t, COLORED);
    }
  };
  auto drawDottedHLine = [&](int y0, int thickness, int onLen, int offLen) {
    for (int t = 0; t < thickness; ++t) {
      for (int x = 0; x < kScreenW; x += (onLen + offLen)) {
        int x1 = x;
        int x2 = x + onLen - 1;
        if (x1 >= kScreenW) break;
        if (x2 >= kScreenW) x2 = kScreenW - 1;
        paint.DrawFilledRectangle(x1, y0 + t, x2, y0 + t, COLORED);
      }
    }
  };
  auto drawDottedVLine = [&](int x0, int thickness, int onLen, int offLen) {
    for (int t = 0; t < thickness; ++t) {
      for (int y = 0; y < kLineH; y += (onLen + offLen)) {
        int y1 = y;
        int y2 = y + onLen - 1;
        if (y1 >= kLineH) break;
        if (y2 >= kLineH) y2 = kLineH - 1;
        paint.DrawFilledRectangle(x0 + t, y1, x0 + t, y2, COLORED);
      }
    }
  };

  int y = 0;

  // ===== Stylized HOLD PAGE HEADER (capsule) =====
  {
    // Build time (HH:MM) from last live string; fallback to local clock
    char hhmm[6] = "--:--";
    if (sLastTimeStr[0]) {
      const char* p = strchr(sLastTimeStr, '|');
      if (p) {
        p++;
        while (*p && !isdigit((unsigned char)*p)) p++;
        int H = 0, M = 0;
        if (sscanf(p, "%2d:%2d", &H, &M) == 2) {
          if (H < 0) H = 0; if (H > 23) H %= 24;
          if (M < 0) M = 0; if (M > 59) M %= 60;
          snprintf(hhmm, sizeof(hhmm), "%02d:%02d", H, M);
        }
      }
    }
    if (hhmm[0] == '-' && hhmm[1] == '-') {
      time_t now = time(nullptr);
      struct tm ti;
      localtime_r(&now, &ti);
      snprintf(hhmm, sizeof(hhmm), "%02d:%02d", ti.tm_hour, ti.tm_min);
    }

    // Capsule metrics
    const int radius = 8;
    const int padX   = 6;
    const int yText  = 5;
    const int topY   = 2;
    const int botY   = kLineH - 3;

    // Pre-calc widths
    char totalBuf[8]; snprintf(totalBuf, sizeof(totalBuf), "%d", sTotalActive);
    const char* totalLabel = "TOTAL";
    int totalLabelW = 5 * fw;
    int totalValW   = (int)strlen(totalBuf) * fw;
    int totalBlockW = totalLabelW + fw /*space*/ + totalValW;

    char pageBuf[16];
    snprintf(pageBuf, sizeof(pageBuf), "PAGE %d/%d", sCurrentPage, sTotalPages);
    int pageW = (int)strlen(pageBuf) * fw;
    int timeW = (int)strlen(hhmm) * fw;
    int centerBlockW = timeW + fw /*space*/ + pageW;

    // Draw capsule outline (no bottom underline)
    paint.Clear(UNCOLORED);
    // NOTE: uses DrawRoundedRectOutline(paint, x, y, w, h, radius, thickness, color)
    DrawRoundedRectOutline(paint, 2, 1, kScreenW - 4, kLineH - 2, radius, 2, COLORED);

    // Left: HOLD
    int xL = padX + 2;
    paint.DrawStringAt(xL, yText, "HOLD", &Font16, COLORED);
    // solid thin vertical after HOLD
    // NOTE: uses DrawVLine(paint, x, yTop, yBot, thickness, color)
    DrawVLine(paint, xL + 4 * fw + 1, topY + 2, botY - 2, 1, COLORED);

    // Center: TIME + PAGE with solid thin separator
    int xC = (kScreenW - centerBlockW) / 2;
    paint.DrawStringAt(xC, yText, hhmm, &Font16, COLORED);
    int xMidLine = xC + timeW + 1;
    DrawVLine(paint, xMidLine, topY + 2, botY - 2, 1, COLORED);
    paint.DrawStringAt(xMidLine + fw, yText, pageBuf, &Font16, COLORED);

    // Right: TOTAL | n (solid thin separator)
    int xR = kScreenW - padX - totalBlockW;
    paint.DrawStringAt(xR, yText, totalLabel, &Font16, COLORED);
    DrawVLine(paint, xR + totalLabelW + 1, topY + 2, botY - 2, 1, COLORED);
    paint.DrawStringAt(xR + totalLabelW + 3, yText, totalBuf, &Font16, COLORED);

    // Push header band (no extra underline)
    epd.Display_Partial_Not_refresh(paint.GetImage(), 0, y, kScreenW, y + kLineH);
    y += kLineH;
  }

  // ===== columns (match LIVE) =====
  const int left = 5;
  const int pad  = 4;           // gap before dotted line
  const int spaceAfterDot = fw; // exactly one "space" after dotted vertical

  const int col1ch = 7;   // callsign (fixed)
  const int col2ch = 7;   // distance "xxx.xkm" (supports 999.9)
  const int col3ch = 5;   // direction "DDDcc"

  int x1_text = left;
  int x1_line = x1_text + col1ch * fw + pad;
  int x2_text = x1_line + spaceAfterDot;
  int x2_line = x2_text + col2ch * fw + pad;
  int x3_text = x2_line + spaceAfterDot;
  int x3_line = x3_text + col3ch * fw + pad;
  int x4_text = x3_line + spaceAfterDot; // country starts here

  // ===== rows (2 bands per aircraft) =====
  int shown = 0;
  for (int i = start; i < active && shown < maxShown; i++) {
    const auto& r = rows[i];

    // Callsign (fallback to ICAO) fixed width
    char callsign[9];
    if (r.callsign[0] == '\0') { strncpy(callsign, r.icao24, sizeof(callsign)); callsign[sizeof(callsign)-1] = '\0'; }
    else                       { strncpy(callsign, r.callsign, sizeof(callsign)); callsign[sizeof(callsign)-1] = '\0'; }
    char csPadded[8]; snprintf(csPadded, sizeof(csPadded), "%-7.7s", callsign);

    // Distance up to 999.9 (fixed width "xxx.xkm" → 7 chars)
    float dist = r.distance; if (dist > 999.9f) dist = 999.9f;
    char distStr[10]; snprintf(distStr, sizeof(distStr), "%5.1fkm", dist);

    // Direction "DDDcc"
    int  bInt = (int)r.bearing; if (bInt < 0) bInt += 360; if (bInt > 359) bInt -= 360;
    char brg[4];  snprintf(brg, sizeof(brg), "%3d", bInt);
    char cd[3];   compass2(r.bearing, cd);
    char dirStr[8]; snprintf(dirStr, sizeof(dirStr), "%s%2.2s", brg, cd);

    // Country trimmed to remaining width
    char countryBuf[32];
    strncpy(countryBuf, r.country, sizeof(countryBuf)-1);
    countryBuf[sizeof(countryBuf)-1] = '\0';
    int maxCountryPx = kScreenW - 5 - x4_text;
    int maxCountryCh = (maxCountryPx > 0) ? (maxCountryPx / fw) : 0;
    if ((int)strlen(countryBuf) > maxCountryCh) countryBuf[maxCountryCh] = '\0';

    // --- Model band (solid top, dotted under) ---
    paint.Clear(UNCOLORED);
    if (shown > 0) { // no top line directly under the header capsule
      drawSolidHLine(0, kSepThickness);
    }
    paint.DrawStringAt(left, 5, r.model, &Font16, COLORED);
    drawDottedHLine(kLineH - 1, kDotThickness, kDotOn, kDotOff);
    epd.Display_Partial_Not_refresh(paint.GetImage(), 0, y, kScreenW, y + kLineH);
    y += kLineH;

    // --- Info band (dotted verticals + bottom solid) ---
    paint.Clear(UNCOLORED);

    if (x1_line < kScreenW) drawDottedVLine(x1_line, kDotThickness, kDotOn, kDotOff);
    if (x2_line < kScreenW) drawDottedVLine(x2_line, kDotThickness, kDotOn, kDotOff);
    if (x3_line < kScreenW) drawDottedVLine(x3_line, kDotThickness, kDotOn, kDotOff);

    paint.DrawStringAt(x1_text, 5, csPadded,   &Font16, COLORED);
    paint.DrawStringAt(x2_text, 5, distStr,    &Font16, COLORED);
    paint.DrawStringAt(x3_text, 5, dirStr,     &Font16, COLORED);
    paint.DrawStringAt(x4_text, 5, countryBuf, &Font16, COLORED);

    drawSolidHLine(kLineH - kSepThickness, kSepThickness);

    epd.Display_Partial_Not_refresh(paint.GetImage(), 0, y, kScreenW, y + kLineH);
    y += kLineH;

    shown++;
  }

  // Clear leftovers
  while (y < kScreenH) {
    paint.Clear(UNCOLORED);
    epd.Display_Partial_Not_refresh(paint.GetImage(), 0, y, kScreenW, std::min(y + kLineH, kScreenH));
    y += kLineH;
  }

  epd.TurnOnDisplay_Partial();
}




void pageUp() {
  if (sMode != HOLD_MODE) return;
  recalcPagingFromActive(countActiveAircraft());
  if (sCurrentPage < sTotalPages) sCurrentPage++;
  drawHoldPagePartial();
  //holdMaybeFullRefresh();
}

void pageDown() {
  if (sMode != HOLD_MODE) return;
  recalcPagingFromActive(countActiveAircraft());
  if (sCurrentPage > 1) sCurrentPage--;
  drawHoldPagePartial();
  //holdMaybeFullRefresh();
}

void redrawHoldPageFull() {
  if (sMode != HOLD_MODE) return;
  drawHoldPageFullBuffer();
}

