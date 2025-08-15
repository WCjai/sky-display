#include "display.h"
#include "cache.h"

#include <algorithm>
#include <cstring>
#include <cctype>
#include <cmath>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

extern volatile bool gHoldRequested;

// -------- EPD & buffers --------
Epd epd;
unsigned char full_image[400 / 8 * 300];
Paint full_paint(full_image, 400, 300);

unsigned char image[400 / 8 * 28];
Paint paint(image, 400, 28);

static constexpr int kTimeBandH = 140;               // tall band for the big clock
static unsigned char time_image[400 / 8 * kTimeBandH];
static Paint time_paint(time_image, 400, kTimeBandH);


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

static bool sPanelPrimed = false;   // we have a valid base loaded
static bool sFastLUT     = false;   // panel is in fast/partial init

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

// ---------- Utilities ----------
void ensurePartialPrimed() {
  if (!sPanelPrimed) {
    epd.Init_Fast(Seconds_1S);          // fast LUT for partial updates
    full_paint.Clear(UNCOLORED);
    epd.Display_Base(full_paint.GetImage());  // push a white base once
    sPanelPrimed = true;
    sFastLUT     = true;
  } else if (!sFastLUT) {
    epd.Init_Fast(Seconds_1S);
    sFastLUT = true;
  }
}

int getActiveCount() {
  int cnt = 0;
  if (!gCacheMutex || xSemaphoreTake(gCacheMutex, pdMS_TO_TICKS(50)) != pdTRUE) return 0;
  for (int i = 0; i < MAX_CACHE_SIZE; i++) {
    const auto& e = aircraftCache[i];
    if (e.active && e.distance >= 0 && e.icao24.length() > 0) cnt++;
  }
  xSemaphoreGive(gCacheMutex);
  return cnt;
}

// safe copy helper
static void cpyBound(char* dst, size_t dstsz, const String& src) {
  if (!dst || dstsz == 0) return;
  size_t n = src.length();
  if (n >= dstsz) n = dstsz - 1;
  memcpy(dst, src.c_str(), n);
  dst[n] = '\0';
}

void drawTooManyAircraftScreen(int total) {
  ScopedDispLock _;

  // Make sure we can do a full push quickly
  ensurePartialPrimed();

  full_paint.Clear(UNCOLORED);

  // Big title
  const char* title = "TOO MANY AIRCRAFT";
  int tX = (kScreenW - (int)strlen(title) * Font16.Width) / 2;
  full_paint.DrawStringAt(tX, 30, title, &Font16, COLORED);
  full_paint.DrawStringAt(tX + 1, 30, title, &Font16, COLORED); // bold-ish

  // Count line
  char countLine[48];
  snprintf(countLine, sizeof(countLine), "Total in view: %d", total);
  int cX = (kScreenW - (int)strlen(countLine) * Font16.Width) / 2;
  full_paint.DrawStringAt(cX, 90, countLine, &Font16, COLORED);

  // Hint line
  const char* hint = "HINT: reduce map zoom, then reboot";
  int hX = (kScreenW - (int)strlen(hint) * Font16.Width) / 2;
  full_paint.DrawStringAt(hX, 130, hint, &Font16, COLORED);

  // Footer note so it's obvious we halted
  const char* halted = "Fetching halted";
  int fX = (kScreenW - (int)strlen(halted) * Font16.Width) / 2;
  full_paint.DrawStringAt(fX, kScreenH - Font16.Height - 10, halted, &Font16, COLORED);

  // Single full-frame push (fast LUT)
  epd.Init_Fast(0);
  epd.Display(full_paint.GetImage());
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


// Case-insensitive compare
static bool ieq(const char* a, const char* b) { return strcasecmp(a, b) == 0; }

// Known long→short mappings (kept small; extend as you like)
// Stored in flash (RODATA) because they're const.
struct CountryMap { const char* full; const char* sh; };
static const CountryMap kCountryMap[] = {
  {"United Arab Emirates", "UAE"},
  {"United States of America", "USA"},
  {"United States", "USA"},
  {"United Kingdom of Great Britain and Northern Ireland", "UK"},
  {"United Kingdom", "UK"},
  {"Kingdom of the Netherlands ", "Netherlands"},
  {"Russian Federation", "Russia"},
  {"People's Republic of China", "China"},
  {"Korea, Republic of", "South Korea"},
  {"Republic of Korea", "South Korea"},
  {"Korea, Democratic People's Republic of", "North Korea"},
  {"Viet Nam", "Vietnam"},
  {"Syrian Arab Republic", "Syria"},
  {"Iran, Islamic Republic of", "Iran"},
  {"Moldova, Republic of", "Moldova"},
  {"Tanzania, United Republic of", "Tanzania"},
  {"Bolivia (Plurinational State of)", "Bolivia"},
  {"Lao People's Democratic Republic", "Laos"},
  {"Czech Republic", "Czechia"},
  {"Congo, the Democratic Republic of the", "DRC"},
  {"Congo, Republic of the", "Congo"},
  {"Türkiye", "Turkey"},
  {"Côte d'Ivoire", "Cote d'Ivoire"},
  {"Taiwan, Province of China", "Taiwan"},
  {"Venezuela (Bolivarian Republic of)", "Venezuela"},
  {"Palestine, State of", "Palestine"},
  {"Macedonia, the former Yugoslav Republic of", "North Macedonia"},
};

static const char* mapCountryShort(const char* name) {
  if (!name || !*name) return nullptr;
  for (size_t i = 0; i < sizeof(kCountryMap)/sizeof(kCountryMap[0]); ++i) {
    if (ieq(name, kCountryMap[i].full)) return kCountryMap[i].sh;
  }
  return nullptr;
}

// Build an acronym from the phrase (keeps first letters of words,
// skips very small stopwords). E.g., "United Arab Emirates" -> "UAE".
static void makeAcronym(const char* in, char* out, size_t outsz) {
  if (!in || !*in || outsz == 0) { if (outsz) out[0] = '\0'; return; }
  const char* stop1 = "of"; const char* stop2 = "and"; const char* stop3 = "the";

  size_t n = 0;
  bool inWord = false;
  char word[24]; int wlen = 0;

  auto flushWord = [&](void){
    if (wlen <= 0) return;
    word[wlen] = '\0';
    // lowercase copy for stopword check
    char lw[24]; for (int i=0;i<=wlen && i<24;i++) lw[i] = tolower((unsigned char)word[i]);
    if (!(ieq(lw, stop1) || ieq(lw, stop2) || ieq(lw, stop3))) {
      // take first alphabetic char
      for (int i=0;i<wlen;i++) {
        if (isalpha((unsigned char)word[i])) {
          if (n + 1 < outsz) out[n++] = (char)toupper((unsigned char)word[i]);
          break;
        }
      }
    }
    wlen = 0;
  };

  for (const char* p = in; *p; ++p) {
    char c = *p;
    if (isalpha((unsigned char)c)) {
      if (!inWord) { inWord = true; wlen = 0; }
      if (wlen < (int)sizeof(word)-1) word[wlen++] = c;
    } else {
      if (inWord) { flushWord(); inWord = false; }
    }
  }
  if (inWord) flushWord();

  if (n == 0) { // fallback: first letters/numbers found
    for (const char* p = in; *p && n + 1 < outsz; ++p) {
      if (isalnum((unsigned char)*p)) out[n++] = (char)toupper((unsigned char)*p);
    }
  }
  out[n] = '\0';
}

// Main helper: choose short label and ensure it fits in maxChars.
// 1) map known long names; 2) if too long, acronymize; 3) if still too long, truncate.
static void shortenCountryForWidth(const char* in, char* out, size_t outsz, int maxChars) {
  if (!in) in = "";
  if (outsz == 0) return;

  const char* mapped = mapCountryShort(in);
  const char* src = mapped ? mapped : in;

  // Copy src into out
  size_t len = strnlen(src, outsz - 1);
  memcpy(out, src, len);
  out[len] = '\0';

  if (maxChars <= 0) { out[0] = '\0'; return; }
  if ((int)len <= maxChars) return;

  // Try acronym
  char acro[16];
  makeAcronym(src, acro, sizeof(acro));
  if ((int)strlen(acro) <= maxChars) {
    strncpy(out, acro, outsz-1);
    out[outsz-1] = '\0';
    return;
  }

  // Last resort: hard truncate
  out[maxChars] = '\0';
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

// void drawNoAircraftScreen(time_t timestamp) {
//   ScopedDispLock _;

//   // Apply timezone offset and format strings
//   time_t zoned = timestamp + (TZ_minutes * 60);
//   struct tm* ti = gmtime(&zoned);

//   char dateStr[32];
//   strftime(dateStr, sizeof(dateStr), "%a %Y-%m-%d", ti);

//   char clockStr[16];
//   if (USE_24H) strftime(clockStr, sizeof(clockStr), "%H:%M", ti);
//   else         strftime(clockStr, sizeof(clockStr), "%I:%M %p", ti);

//   int currentY = 0;

//   // --- Date ---
//   paint.Clear(UNCOLORED);
//   int dateX = (kScreenW - (int)strlen(dateStr) * Font16.Width) / 2;
//   paint.DrawStringAt(dateX, 5, dateStr, &Font16, COLORED);
//   epd.Display_Partial_Not_refresh(paint.GetImage(), 0, currentY, kScreenW, currentY + kLineH);
//   currentY += kLineH;

//   // --- Time ---
//   int clockY = (kScreenH - Font24.Height) / 2;
//   paint.Clear(UNCOLORED);
//   int clockX = (kScreenW - (int)strlen(clockStr) * Font24.Width) / 2;
//   paint.DrawStringAt(clockX,     5, clockStr, &Font24, COLORED);
//   paint.DrawStringAt(clockX + 1, 5, clockStr, &Font24, COLORED);  // fake bold
//   epd.Display_Partial_Not_refresh(paint.GetImage(), 0, clockY, kScreenW, clockY + kLineH);

//   // --- Footer ---
//   const char* footer = "no aircraft nearby";
//   int footerY = kScreenH - kLineH;
//   paint.Clear(UNCOLORED);
//   int footerX = (kScreenW - (int)strlen(footer) * Font16.Width) / 2;
//   paint.DrawStringAt(footerX, 5, footer, &Font16, COLORED);
//   epd.Display_Partial_Not_refresh(paint.GetImage(), 0, footerY, kScreenW, footerY + kLineH);

//   // --- Apply partial refresh ---
//   epd.TurnOnDisplay_Partial();
// }

// === Add these helpers near the top of display.cpp (after includes) ===
static void draw7SegDigit(Paint& p, int x, int y, int W, int H, int T, int color, int d) {
  enum { A=1<<0, B=1<<1, C=1<<2, D=1<<3, E=1<<4, F=1<<5, G=1<<6 };
  static const uint8_t mask[10] = {
    (uint8_t)(A|B|C|D|E|F),          // 0
    (uint8_t)(B|C),                  // 1
    (uint8_t)(A|B|G|E|D),            // 2
    (uint8_t)(A|B|G|C|D),            // 3
    (uint8_t)(F|G|B|C),              // 4
    (uint8_t)(A|F|G|C|D),            // 5
    (uint8_t)(A|F|G|E|C|D),          // 6
    (uint8_t)(A|B|C),                // 7
    (uint8_t)(A|B|C|D|E|F|G),        // 8
    (uint8_t)(A|B|C|D|F|G)           // 9
  };
  if (d < 0 || d > 9) return;
  uint8_t m = mask[d];

  int midY = y + H/2;

  auto HSeg = [&](int x1, int x2, int yy) {
    p.DrawFilledRectangle(x1, yy, x2, yy + T - 1, color);
  };
  auto VSeg = [&](int xx, int y1, int y2) {
    p.DrawFilledRectangle(xx, y1, xx + T - 1, y2, color);
  };

  if (m & A) HSeg(x + T,       x + W - T - 1, y);           // top
  if (m & D) HSeg(x + T,       x + W - T - 1, y + H - T);   // bottom
  if (m & G) HSeg(x + T,       x + W - T - 1, midY - T/2);  // middle

  if (m & F) VSeg(x,           y + T,         midY - 1);           // top-left
  if (m & E) VSeg(x,           midY,          y + H - T - 1);      // bottom-left
  if (m & B) VSeg(x + W - T,   y + T,         midY - 1);           // top-right
  if (m & C) VSeg(x + W - T,   midY,          y + H - T - 1);      // bottom-right
}

static void drawColon(Paint& p, int x, int y, int H, int dot, int gap, int color) {
  int midY = y + H/2;
  // top dot
  p.DrawFilledRectangle(x, midY - gap - dot, x + dot - 1, midY - gap - 1, color);
  // bottom dot
  p.DrawFilledRectangle(x, midY + gap,       x + dot - 1, midY + gap + dot - 1, color);
}


// === Replace your whole drawNoAircraftScreen(...) with this ===
// helper: push white stripes to clear an arbitrary vertical range
static inline void pushWhiteBands(int y0, int y1) {
  if (y1 <= y0) return;
  for (int y = y0; y < y1; y += kLineH) {
    int y2 = y + kLineH;
    if (y2 > y1) y2 = y1;
    paint.Clear(UNCOLORED);
    epd.Display_Partial_Not_refresh(paint.GetImage(), 0, y, kScreenW, y2);
  }
}

// Put this helper near the top of display.cpp (once)
static inline void to12h(int hour24, int& displayHour, char ampm[3]) {
  if (hour24 < 0) hour24 = 0;
  bool pm = (hour24 >= 12);
  int h = hour24 % 12;
  if (h == 0) h = 12;
  displayHour = h;
  ampm[0] = pm ? 'P' : 'A';
  ampm[1] = 'M';
  ampm[2] = '\0';
}

// Keep your draw7SegDigit, drawColon, time_paint, and pushWhiteBands(..) from before.

void drawNoAircraftScreen(time_t timestamp) {
  ScopedDispLock _;
  ensurePartialPrimed();

  // ----- build date/time -----
  time_t zoned = timestamp + (TZ_minutes * 60);
  struct tm* ti = gmtime(&zoned);

  char dateStr[32];
  strftime(dateStr, sizeof(dateStr), "%a %Y-%m-%d", ti);

  const bool is24 = USE_24H;
  int  hour24 = ti->tm_hour;
  int  min    = ti->tm_min;

  // throttle: only update once per minute (but draw the first time)
  static bool firstDraw = true;
  static int  lastMin = -1, lastHour24 = -1;
  bool timeChanged = firstDraw || (min != lastMin) || (hour24 != lastHour24);
  if (!timeChanged) return;

  // --- 12h conversion + AM/PM ---
  int dispHour = hour24;
  char ampm[3] = "";
  if (!is24) {
    bool pm = (hour24 >= 12);
    int h = hour24 % 12; if (h == 0) h = 12;
    dispHour = h;
    ampm[0] = pm ? 'P' : 'A'; ampm[1] = 'M'; ampm[2] = '\0';
  }

  // split digits
  int hT = dispHour / 10, hO = dispHour % 10;
  int mT = min / 10,      mO = min % 10;

  // ----- geometry (center entire block: digits + optional AM/PM) -----
  const int D_W       = 70;               // digit width
  const int D_H       = kTimeBandH;       // digit height (e.g., 140)
  const int SEG_T     = 12;               // segment thickness
  const int GAP_DIG   = 12;               // gap between digits

  const int COLON_W   = SEG_T;            // colon width
  const int AMPM_GAP  = 10;               // gap between minutes and AM/PM

  // Dynamic colon spacing:
  // - single-digit 12h hour -> tight on right to avoid "3: 18"
  // - otherwise (24h or 12h two-digit hour) -> symmetric gaps
  bool twoDigitHour    = is24 ? true : (dispHour >= 10);
  const int GAP_COLON_L = 10;
  const int GAP_COLON_R = twoDigitHour ? 10 : 0;

  bool hideLeadingZero = (!is24 && !twoDigitHour); // hide only for 12h single-digit

  // compute total width
  int digitsW = 0;
  if (!hideLeadingZero) { digitsW += D_W; digitsW += GAP_DIG; }  // H_t + gap
  digitsW += D_W;                                                // H_o
  digitsW += GAP_COLON_L + COLON_W + GAP_COLON_R;                // colon + gaps
  digitsW += D_W + GAP_DIG + D_W;                                // M_t + gap + M_o

  int ampmW = (!is24) ? (AMPM_GAP + (int)strlen(ampm) * Font16.Width) : 0;

  int totalW = digitsW + ampmW;
  int startX = (kScreenW - totalW) / 2;     // horizontal centering
  int startY = (kScreenH - D_H) / 2;        // vertical centering

  // ============================
  // 1) HEADER band (always upload)
  // ============================
  paint.Clear(UNCOLORED);
  int dateX = (kScreenW - (int)strlen(dateStr) * Font16.Width) / 2;
  paint.DrawStringAt(dateX, 6, dateStr, &Font16, COLORED);
  epd.Display_Partial_Not_refresh(paint.GetImage(), 0, 0, kScreenW, kLineH);

  // ============================
  // 2) CLEAR top gap area (wipe LIVE leftovers)
  // ============================
  auto pushWhiteBands = [&](int y0, int y1) {
    if (y1 <= y0) return;
    for (int y = y0; y < y1; y += kLineH) {
      int y2 = y + kLineH; if (y2 > y1) y2 = y1;
      paint.Clear(UNCOLORED);
      epd.Display_Partial_Not_refresh(paint.GetImage(), 0, y, kScreenW, y2);
    }
  };
  pushWhiteBands(kLineH, startY);

  // ============================
  // 3) TIME band (centered, includes AM/PM width)
  // ============================
  time_paint.Clear(UNCOLORED);
  int x = startX;

  if (!hideLeadingZero) {
    draw7SegDigit(time_paint, x, 0, D_W, D_H, SEG_T, COLORED, hT);
    x += D_W + GAP_DIG;
  }

  draw7SegDigit(time_paint, x, 0, D_W, D_H, SEG_T, COLORED, hO);
  x += D_W + GAP_COLON_L;

  // colon
  drawColon(time_paint, x + (COLON_W - SEG_T)/2, 0, D_H, SEG_T/2 + 2, 10, COLORED);
  x += COLON_W + GAP_COLON_R;

  draw7SegDigit(time_paint, x, 0, D_W, D_H, SEG_T, COLORED, mT);
  x += D_W + GAP_DIG;

  draw7SegDigit(time_paint, x, 0, D_W, D_H, SEG_T, COLORED, mO);
  x += D_W;

  if (!is24) {
    int ampmY = (D_H - Font16.Height) / 2;
    time_paint.DrawStringAt(x + AMPM_GAP, ampmY, ampm, &Font16, COLORED);
  }

  epd.Display_Partial_Not_refresh(time_paint.GetImage(), 0, startY, kScreenW, startY + D_H);

  // ============================
  // 4) CLEAR bottom gap area & 5) FOOTER band
  // ============================
  int footerY  = kScreenH - kLineH;
  pushWhiteBands(startY + D_H, footerY);

  const char* footer = "no aircraft nearby";
  paint.Clear(UNCOLORED);
  int footerX = (kScreenW - (int)strlen(footer) * Font16.Width) / 2;
  paint.DrawStringAt(footerX, 5, footer, &Font16, COLORED);
  epd.Display_Partial_Not_refresh(paint.GetImage(), 0, footerY, kScreenW, footerY + kLineH);

  // ============================
  // 6) Single partial refresh
  // ============================
  epd.TurnOnDisplay_Partial();

  // remember last shown time
  firstDraw  = false;
  lastMin    = min;
  lastHour24 = hour24;
}





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

  // crude quarter arcs (symmetric pixels)
  for (int i = 0; i < r; ++i) {
    int dx = r - i;
    int dy = (int)roundf((float)std::sqrt((float)r * r - dx * dx));
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

// -------- LIVE (batched partial, single refresh) --------
void drawAircraftInfoToDisplay_Partial(const char* timeStr, int totalAircraftFromAPI) {
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

    int totalToShow = (totalAircraftFromAPI > 0) ? totalAircraftFromAPI : sTotalActive;
    char totalBuf[8]; snprintf(totalBuf, sizeof(totalBuf), "%d", totalToShow);
    const char* totalLabel = "TOTAL";
    int totalLabelW = 5 * fw;                   // "TOTAL"
    int totalValW   = (int)strlen(totalBuf) * fw;
    int totalBlockW = totalLabelW + fw /* 1 space */ + totalValW;

    // Clear and draw capsule (no bottom line)
    paint.Clear(UNCOLORED);
    DrawRoundedRectOutline(paint, 2, 1, kScreenW - 4, kLineH - 2, radius, 2, COLORED);

    // ---- Left: DATE (YYYY | MM | DD) ----
    int xL = padX + 4;
    paint.DrawStringAt(xL, yText, yy, &Font16, COLORED);
    DrawVLine(paint, xL + 4 * fw + 1, topY + 2, botY - 2, 1, COLORED);
    int xMM = xL + 4 * fw + 3;
    paint.DrawStringAt(xMM, yText, mm, &Font16, COLORED);
    DrawVLine(paint, xMM + 2 * fw + 1, topY + 2, botY - 2, 1, COLORED);
    int xDD = xMM + 2 * fw + 3;
    paint.DrawStringAt(xDD, yText, dd, &Font16, COLORED);

    // ---- Center: TIME (HH:MM:SS [AM/PM]) ----
    int xC = (kScreenW - timeW) / 2;
    paint.DrawStringAt(xC, yText, hhmmss, &Font16, COLORED);
    if (haveAMPM) {
      DrawVLine(paint, xC + 8 * fw + 1, topY + 2, botY - 2, 1, COLORED);
      paint.DrawStringAt(xC + 8 * fw + 3, yText, ampm, &Font16, COLORED);
    }

    // ---- Right: TOTAL | n ----
    int xR = kScreenW - padX - totalBlockW;
    paint.DrawStringAt(xR, yText, totalLabel, &Font16, COLORED);
    DrawVLine(paint, xR + totalLabelW + 1, topY + 2, botY - 2, 1, COLORED);
    paint.DrawStringAt(xR + totalLabelW + 3, yText, totalBuf, &Font16, COLORED);

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

    // Distance — fixed width 7 chars ("xxx.xkm" or integer "   xxxkm")
    float dist = r.distance;
    char distStr[10];

    if (dist > 99.9f) {
        //snprintf(distStr, sizeof(distStr), "%3dkm", (int)dist); // integer format
        snprintf(distStr, sizeof(distStr), "%4.1fkm", dist);
    } else {
        snprintf(distStr, sizeof(distStr), "%4.1fkm", dist);     // one decimal place
    }

    // Direction — "dddXY"
    int  bInt = (int)r.bearing; if (bInt < 0) bInt += 360; if (bInt > 359) bInt -= 360;
    char brg[4];  snprintf(brg, sizeof(brg), "%3d", bInt);
    char cd[3];   compass2(r.bearing, cd);
    char dirStr[8]; snprintf(dirStr, sizeof(dirStr), "%s%s", brg, cd);

    // --- Model band ---
    paint.Clear(UNCOLORED);

    // Remove the top border if this is the very first row after the header
    const int kSepThickness = 1;
    const int kDotThickness = 1;
    const int kDotOn = 4, kDotOff = 3;

    if (shown > 0) {  // no top line directly under the header capsule
      for (int t = 0; t < kSepThickness; ++t)
        paint.DrawFilledRectangle(0, 0 + t, kScreenW - 1, 0 + t, COLORED);
    }

    paint.DrawStringAt(5, 5, r.model, &Font16, COLORED);
    for (int t = 0; t < kDotThickness; ++t) {
      for (int x = 0; x < kScreenW; x += (kDotOn + kDotOff)) {
        int x1 = x;
        int x2 = x + kDotOn - 1;
        if (x1 >= kScreenW) break;
        if (x2 >= kScreenW) x2 = kScreenW - 1;
        paint.DrawFilledRectangle(x1, kLineH - 1 + t, x2, kLineH - 1 + t, COLORED);
      }
    }
    epd.Display_Partial_Not_refresh(paint.GetImage(), 0, y, kScreenW, y + kLineH);
    y += kLineH; linesUsed++;

    // --- Info band with dotted verticals ---
    paint.Clear(UNCOLORED);

    const int fw = Font16.Width;
    const int left = 5;
    const int pad  = 4;
    const int spaceAfterDot = fw; // +1 char

    const int col1ch = 7;   // callsign
    const int col2ch = 7;   // distance
    const int col3ch = 5;   // direction

    int x1_text = left;
    int x1_line = x1_text + col1ch * fw + pad;
    int x2_text = x1_line + spaceAfterDot;
    int x2_line = x2_text + col2ch * fw + pad;
    int x3_text = x2_line + spaceAfterDot;
    int x3_line = x3_text + col3ch * fw + pad;
    int x4_text = x3_line + spaceAfterDot;

    // Country fits remaining width
    const int rightPad = 5;                          // keep a small right margin
    int maxCountryPx = kScreenW - rightPad - x4_text;
    int maxCountryCh = (maxCountryPx > 0) ? (maxCountryPx / fw) : 0;

    char countryBuf[32];
    shortenCountryForWidth(r.country, countryBuf, sizeof(countryBuf), maxCountryCh);
    // no need to truncate again; helper already ensures <= maxCountryCh

    paint.DrawStringAt(x1_text, 5, csPadded,   &Font16, COLORED);
    paint.DrawStringAt(x2_text, 5, distStr,    &Font16, COLORED);
    paint.DrawStringAt(x3_text, 5, dirStr,     &Font16, COLORED);
    paint.DrawStringAt(x4_text, 5, countryBuf, &Font16, COLORED);


    // vertical dotted lines
    auto drawDottedVLine = [&](int x0, int thickness, int onLen, int offLen) {
      for (int t = 0; t < thickness; ++t) {
        for (int yy = 0; yy < kLineH; yy += (onLen + offLen)) {
          int y1 = yy;
          int y2 = yy + onLen - 1;
          if (y1 >= kLineH) break;
          if (y2 >= kLineH) y2 = kLineH - 1;
          paint.DrawFilledRectangle(x0 + t, y1, x0 + t, y2, COLORED);
        }
      }
    };
    if (x1_line < kScreenW) drawDottedVLine(x1_line, kDotThickness, kDotOn, kDotOff);
    if (x2_line < kScreenW) drawDottedVLine(x2_line, kDotThickness, kDotOn, kDotOff);
    if (x3_line < kScreenW) drawDottedVLine(x3_line, kDotThickness, kDotOn, kDotOff);

    // bottom border
    for (int t = 0; t < kSepThickness; ++t)
      paint.DrawFilledRectangle(0, kLineH - kSepThickness + t, kScreenW - 1, kLineH - kSepThickness + t, COLORED);

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

// -------- HOLD (partial page renderer) --------
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
    const char* totalLabel = "SAVED";
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
    DrawRoundedRectOutline(paint, 2, 1, kScreenW - 4, kLineH - 2, radius, 2, COLORED);

    // Left: HOLD
    int xL = padX + 2;
    paint.DrawStringAt(xL, yText, "HOLD", &Font16, COLORED);
    DrawVLine(paint, xL + 4 * fw + 1, topY + 2, botY - 2, 1, COLORED);

    // Center: TIME + PAGE
    int xC = (kScreenW - centerBlockW) / 2;
    paint.DrawStringAt(xC, yText, hhmm, &Font16, COLORED);
    int xMidLine = xC + timeW + 1;
    DrawVLine(paint, xMidLine, topY + 2, botY - 2, 1, COLORED);
    paint.DrawStringAt(xMidLine + fw, yText, pageBuf, &Font16, COLORED);

    // Right: TOTAL | n
    int xR = kScreenW - padX - totalBlockW;
    paint.DrawStringAt(xR, yText, totalLabel, &Font16, COLORED);
    DrawVLine(paint, xR + totalLabelW + 1, topY + 2, botY - 2, 1, COLORED);
    paint.DrawStringAt(xR + totalLabelW + 3, yText, totalBuf, &Font16, COLORED);

    epd.Display_Partial_Not_refresh(paint.GetImage(), 0, y, kScreenW, y + kLineH);
    y += kLineH;
  }

  // ===== columns (match LIVE) =====
  const int left = 5;
  const int pad  = 4;           // gap before dotted line
  const int spaceAfterDot = fw; // exactly one "space" after dotted vertical

  const int col1ch = 7;   // callsign (fixed)
  const int col2ch = 7;   // distance "xxx.xkm"
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

    // Distance fixed width 7 chars
    float dist = r.distance;
    char distStr[10];

    if (dist > 99.9f) {
        //snprintf(distStr, sizeof(distStr), "%3dkm", (int)dist); // integer format
        snprintf(distStr, sizeof(distStr), "%4.1fkm", dist);
    } else {
        snprintf(distStr, sizeof(distStr), "%4.1fkm", dist);     // one decimal place
    }

    // Direction "DDDcc"
    int  bInt = (int)r.bearing; if (bInt < 0) bInt += 360; if (bInt > 359) bInt -= 360;
    char brg[4];  snprintf(brg, sizeof(brg), "%3d", bInt);
    char cd[3];   compass2(r.bearing, cd);
    char dirStr[8]; snprintf(dirStr, sizeof(dirStr), "%s%2.2s", brg, cd);

    // Country trimmed to remaining width
    const int rightPad = 5;                          // keep a small right margin
    int maxCountryPx = kScreenW - rightPad - x4_text;
    int maxCountryCh = (maxCountryPx > 0) ? (maxCountryPx / fw) : 0;

    char countryBuf[32];
    shortenCountryForWidth(r.country, countryBuf, sizeof(countryBuf), maxCountryCh);

    // --- Model band (solid top, dotted under) ---
    paint.Clear(UNCOLORED);
    if (shown > 0) {
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

// -------- Mode & paging API --------
DisplayMode getDisplayMode() { return sMode; }

void setDisplayMode(DisplayMode m) {
  // Guard: don’t enter HOLD when <5 aircraft
  if (m == HOLD_MODE && getActiveCount() < 6) return;

  if (sMode == m) {
    // If pressing again in HOLD, redraw current page
    if (m == HOLD_MODE) {
      recalcPagingFromActive(getActiveCount());
      sCurrentPage = std::max(1, std::min(sCurrentPage, sTotalPages));
      ensurePartialPrimed();
      drawHoldPagePartial();
    }
    return;
  }

  // SWITCH
  sMode = m;

  if (sMode == HOLD_MODE) {
    ensurePartialPrimed();
    recalcPagingFromActive(getActiveCount());
    sCurrentPage = 1;
    drawHoldPagePartial();                // partial-only
  } else {
    // LIVE mode: partial-only as well (no Clear)
    ensurePartialPrimed();
    drawAircraftInfoToDisplay_Partial(sLastTimeStr, -1);
  }
}

void pageUp() {
  if (sMode != HOLD_MODE) return;
  recalcPagingFromActive(getActiveCount());
  if (sCurrentPage < sTotalPages) sCurrentPage++;
  drawHoldPagePartial();
}

void pageDown() {
  if (sMode != HOLD_MODE) return;
  recalcPagingFromActive(getActiveCount());
  if (sCurrentPage > 1) sCurrentPage--;
  drawHoldPagePartial();
}

// Optional: full-frame HOLD (kept for API completeness)
static void drawHoldPageFullBuffer() {
  RowView rows[MAX_CACHE_SIZE];
  int active = snapshotActiveRows(rows, MAX_CACHE_SIZE);
  std::sort(rows, rows + active, [](const RowView& a, const RowView& b){
    return a.distance < b.distance;
  });
  recalcPagingFromActive(active);

  ScopedDispLock _;

  // Simple header (HH:MM fallback)
  char hhmm[6] = "--:--";
  if (sLastTimeStr[0]) {
    const char* p = strchr(sLastTimeStr, '|');
    if (p) {
      p++; while (*p && !isdigit((unsigned char)*p)) p++;
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

  full_paint.Clear(UNCOLORED);

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

    // Callsign (fallback to ICAO)
    char callsign[9];
    if (r.callsign[0] == '\0') { strncpy(callsign, r.icao24, sizeof(callsign)); callsign[sizeof(callsign)-1] = '\0'; }
    else                       { strncpy(callsign, r.callsign, sizeof(callsign)); callsign[sizeof(callsign)-1] = '\0'; }
    char csPadded[8];  snprintf(csPadded, sizeof(csPadded), "%-7.7s", callsign);

    float dist = r.distance;
    char distPadded[12];
    if (dist > 99.9f) snprintf(distPadded, sizeof(distPadded), "%7dkm", (int)dist);
    else              snprintf(distPadded, sizeof(distPadded), "%7.1f", dist);

    int   bInt = (int)r.bearing; if (bInt < 0) bInt += 360; if (bInt > 359) bInt -= 360;
    char  brg[4]; snprintf(brg, sizeof(brg), "%3d", bInt);

    char cd[3]; compass2(r.bearing, cd);

    char infoLine[96];
    snprintf(infoLine, sizeof(infoLine), "%s%skm %s%s %s",
             csPadded, distPadded, brg, cd, r.country);

    // alternating bands
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

  epd.Init_Fast(0);
  epd.Display(full_paint.GetImage());
}

void redrawHoldPageFull() {
  if (sMode != HOLD_MODE) return;
  drawHoldPageFullBuffer();
}
