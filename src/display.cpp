// display.cpp

#include "display.h"
#include "cache.h"
#include "digits.h"
#include <algorithm>
#include <cstring>
#include <cctype>
#include <cmath>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

extern volatile bool gHoldRequested; // defined in main.cpp
#define DEBUG_BOOT_DUMMY_LIVE 0
#define DEBUG_BOOT_DUMMY_HOLD 1
#define USE_BITMAP_CLOCK 1
// -------- EPD & buffers --------
Epd epd;
unsigned char full_image[400 / 8 * 300];
Paint full_paint(full_image, 400, 300);

unsigned char image[400 / 8 * 28];
Paint paint(image, 400, 28);

static constexpr int kTimeBandH = 140;               // tall band for big clock
static unsigned char time_image[400 / 8 * kTimeBandH];
static Paint time_paint(time_image, 400, kTimeBandH);

// ------ 16x16 monochrome icons (1 = BLACK, 0 = WHITE, MSB-first) ------
static const uint8_t ICON_CAL_16[32] PROGMEM = { /* 0X00,0X01,0X0F,0X00,0X10,0X00, */
0X08,0X20,0X6B,0XAC,0XEB,0XAE,0XE7,0XCE,0XFF,0XFE,0XFF,0XFE,0X80,0X02,0X80,0X02,
0X9B,0XB2,0X9B,0XB2,0X80,0X02,0X9B,0XB2,0X9B,0XB2,0X80,0X02,0X80,0X02,0XFF,0XFE,
};

static const uint8_t ICON_CLK_16[32] PROGMEM = { /* 0X00,0X01,0X10,0X00,0X0F,0X00, */
0X07,0XE0,0X1F,0XF8,0X39,0X9C,0X61,0X86,0X61,0X86,0XC1,0X83,0XC1,0X83,0XC1,0X83,
0XC0,0XE3,0XC0,0X3B,0X60,0X06,0X60,0X06,0X38,0X1C,0X1F,0XF8,0X07,0XE0,};

static const uint8_t ICON_PLN_16[32] PROGMEM = { /* 0X00,0X01,0X0F,0X00,0X10,0X00, */
0X00,0X06,0X70,0X0E,0X3D,0X9C,0X0F,0XB8,0X07,0XF8,0X01,0XF0,0X01,0XF8,0X03,0XF8,
0X07,0XF8,0X0F,0X38,0XCE,0X18,0X7C,0X18,0X38,0X0C,0X38,0X04,0X08,0X04,0X08,0X00,
};


// 16x16 monochrome icons (MSB-first). 1 = BLACK pixel
static const uint8_t ICON_PAUSE_16[32] PROGMEM = { /* 0X00,0X01,0X0F,0X00,0X10,0X00, */
0X07,0XC0,0X1F,0XF0,0X3C,0X78,0X70,0X1C,0X66,0XCC,0XE6,0XC6,0XC6,0XC6,0XC6,0XC6,
0XC6,0XC6,0XC6,0XC6,0XE6,0XCE,0X60,0X0C,0X70,0X1C,0X3C,0X78,0X1F,0XF0,0X07,0XC0,
};

static const uint8_t ICON_PAGE_16[32] PROGMEM = { /* 0X00,0X01,0X10,0X00,0X10,0X00, */
0X3F,0XE0,0X20,0X30,0X20,0X38,0X20,0X2C,0X20,0X3C,0X20,0X04,0X20,0X04,0X23,0XF4,
0X20,0X04,0X23,0XF4,0X20,0X04,0X20,0X04,0X23,0XF4,0X20,0X04,0X20,0X04,0X3F,0XFC,
};


static const uint8_t ICON_SAVE_16[32] PROGMEM = { /* 0X00,0X01,0X0F,0X00,0X10,0X00, */
0XFF,0XF8,0XF0,0X6C,0XF0,0X6E,0XF0,0X6E,0XF0,0X6E,0XF0,0X6E,0XFF,0XFE,0XFF,0XFE,
0XC0,0X0E,0XC0,0X0E,0XC0,0X0E,0XC0,0X0E,0XC0,0X0E,0XC0,0X0E,0XC0,0X0E,0XFF,0XFE,
};


// -------- Display mutex --------
SemaphoreHandle_t gDisplayMutex = nullptr;

void initDisplayMutex() {
  if (!gDisplayMutex) {
    gDisplayMutex = xSemaphoreCreateMutex();
  }
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

static constexpr int HEADER_TEXT_Y_ADJ = 2;   // try 1; set 0 if you prefer
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

// --------- STATIC BUFFERS (moved off task stacks) ---------
static RowView s_rows_live[MAX_CACHE_SIZE];
static RowView s_rows_hold[MAX_CACHE_SIZE];
static RowView s_rows_full[MAX_CACHE_SIZE];

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

  ensurePartialPrimed();
  full_paint.Clear(UNCOLORED);

  const char* title = "TOO MANY AIRCRAFT";
  int tX = (kScreenW - (int)strlen(title) * Font16.Width) / 2;
  full_paint.DrawStringAt(tX, 30, title, &Font16, COLORED);
  full_paint.DrawStringAt(tX + 1, 30, title, &Font16, COLORED);

  char countLine[48];
  snprintf(countLine, sizeof(countLine), "Total in view: %d", total);
  int cX = (kScreenW - (int)strlen(countLine) * Font16.Width) / 2;
  full_paint.DrawStringAt(cX, 90, countLine, &Font16, COLORED);

  const char* hint = "HINT: reduce map zoom, then reboot";
  int hX = (kScreenW - (int)strlen(hint) * Font16.Width) / 2;
  full_paint.DrawStringAt(hX, 130, hint, &Font16, COLORED);

  const char* halted = "Fetching halted";
  int fX = (kScreenW - (int)strlen(halted) * Font16.Width) / 2;
  full_paint.DrawStringAt(fX, kScreenH - Font16.Height - 10, halted, &Font16, COLORED);

  epd.Init_Fast(0);
  epd.Display(full_paint.GetImage());
}

// get 2-letter compass suffix without heap allocs
static void compass2(float bearing, char out[3]) {
  static const char table[8][3] = {"N ","NE","E ","SE","S ","SW","W ","NW"};
  int idx = (int)lroundf(bearing / 45.0f);
  if (idx < 0) idx = 0;
  if (idx > 8) idx = 8;
  if (idx == 8) idx = 0;
  out[0] = table[idx][0];
  out[1] = table[idx][1];
  out[2] = '\0';
}

// Case-insensitive compare
static bool ieq(const char* a, const char* b) { return strcasecmp(a, b) == 0; }
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

// Build an acronym from the phrase (keeps first letters of words)
static void makeAcronym(const char* in, char* out, size_t outsz) {
  if (!in || !*in || outsz == 0) { if (outsz) out[0] = '\0'; return; }
  const char* stop1 = "of"; const char* stop2 = "and"; const char* stop3 = "the";
  size_t n = 0; bool inWord = false; char word[24]; int wlen = 0;
  auto flushWord = [&](){
    if (wlen <= 0) return;
    word[wlen] = '\0';
    char lw[24]; for (int i=0;i<=wlen && i<24;i++) lw[i] = tolower((unsigned char)word[i]);
    if (!(ieq(lw, stop1) || ieq(lw, stop2) || ieq(lw, stop3))) {
      for (int i=0;i<wlen;i++) if (isalpha((unsigned char)word[i])) {
        if (n + 1 < outsz) out[n++] = (char)toupper((unsigned char)word[i]);
        break;
      }
    }
    wlen = 0;
  };
  for (const char* p = in; *p; ++p) {
    char c = *p;
    if (isalpha((unsigned char)c)) { if (!inWord) { inWord = true; wlen = 0; } if (wlen < (int)sizeof(word)-1) word[wlen++] = c; }
    else { if (inWord) { flushWord(); inWord = false; } }
  }
  if (inWord) flushWord();
  if (n == 0) {
    for (const char* p = in; *p && n + 1 < outsz; ++p) if (isalnum((unsigned char)*p)) out[n++] = (char)toupper((unsigned char)*p);
  }
  out[n] = '\0';
}

// Shorten country to fit width
static void shortenCountryForWidth(const char* in, char* out, size_t outsz, int maxChars) {
  if (!in) in = "";
  if (outsz == 0) return;
  const char* mapped = mapCountryShort(in);
  const char* src = mapped ? mapped : in;
  size_t len = strnlen(src, outsz - 1);
  memcpy(out, src, len);
  out[len] = '\0';
  if (maxChars <= 0) { out[0] = '\0'; return; }
  if ((int)len <= maxChars) return;
  char acro[16]; makeAcronym(src, acro, sizeof(acro));
  if ((int)strlen(acro) <= maxChars) { strncpy(out, acro, outsz-1); out[outsz-1] = '\0'; return; }
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

// === 7-segment helpers ===
static void draw7SegDigit(Paint& p, int x, int y, int W, int H, int T, int color, int d) {
  enum { A=1<<0, B=1<<1, C=1<<2, D=1<<3, E=1<<4, F=1<<5, G=1<<6 };
  static const uint8_t mask[10] = {
    (uint8_t)(A|B|C|D|E|F), (uint8_t)(B|C), (uint8_t)(A|B|G|E|D),
    (uint8_t)(A|B|G|C|D),   (uint8_t)(F|G|B|C), (uint8_t)(A|F|G|C|D),
    (uint8_t)(A|F|G|E|C|D), (uint8_t)(A|B|C), (uint8_t)(A|B|C|D|E|F|G),
    (uint8_t)(A|B|C|D|F|G)
  };
  if (d < 0 || d > 9) return;
  uint8_t m = mask[d];

  int midY = y + H/2;

  auto HSeg = [&](int x1, int x2, int yy) { p.DrawFilledRectangle(x1, yy, x2, yy + T - 1, color); };
  auto VSeg = [&](int xx, int y1, int y2) { p.DrawFilledRectangle(xx, y1, xx + T - 1, y2, color); };

  if (m & A) HSeg(x + T,       x + W - T - 1, y);
  if (m & D) HSeg(x + T,       x + W - T - 1, y + H - T);
  if (m & G) HSeg(x + T,       x + W - T - 1, midY - T/2);
  if (m & F) VSeg(x,           y + T,         midY - 1);
  if (m & E) VSeg(x,           midY,          y + H - T - 1);
  if (m & B) VSeg(x + W - T,   y + T,         midY - 1);
  if (m & C) VSeg(x + W - T,   midY,          y + H - T - 1);
}

static void drawColon(Paint& p, int x, int y, int H, int dot, int gap, int color) {
  int midY = y + H/2;
  p.DrawFilledRectangle(x, midY - gap - dot, x + dot - 1, midY - gap - 1, color);
  p.DrawFilledRectangle(x, midY + gap,       x + dot - 1, midY + gap + dot - 1, color);
}

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

// 12h conversion
static inline void to12h(int hour24, int& displayHour, char ampm[3]) {
  if (hour24 < 0) hour24 = 0;
  bool pm = (hour24 >= 12);
  int h = hour24 % 12; if (h == 0) h = 12;
  displayHour = h;
  ampm[0] = pm ? 'P' : 'A';
  ampm[1] = 'M';
  ampm[2] = '\0';
}

static void BlitGlyph1bpp_MSBL(int x, int y,
                               int visible_w_bits,  // e.g., 70
                               int stride_bits,     // e.g., 72 (9 bytes)
                               int h,
                               const uint8_t* src)
{
  if (!src || visible_w_bits <= 0 || h <= 0 || stride_bits <= 0 || (stride_bits & 7)) return;

  const int row_bytes = stride_bits / 8;

  // Prepare the push buffer exactly stride_bits wide
  const int oldW = time_paint.GetWidth();
  const int oldH = time_paint.GetHeight();
  time_paint.SetWidth(stride_bits);
  time_paint.SetHeight(h);

  uint8_t* dst = time_paint.GetImage();
  memset(dst, 0xFF, row_bytes * h);  // white

  // Precompute mask for the last byte so padding bits stay white.
  const int rem = visible_w_bits & 7;                 // 70 % 8 = 6
  const bool need_mask = (rem != 0);
  const uint8_t last_mask = need_mask ? (uint8_t)(0xFF & (~((1u << (8 - rem)) - 1)))  // MSB-first -> keep top 'rem' bits
                                      : 0xFF;                                         // full byte

  for (int row = 0; row < h; ++row) {
    const uint8_t* s = src + row * row_bytes;
    uint8_t*       d = dst + row * row_bytes;

    // copy all full bytes except the last
    const int full_bytes = (visible_w_bits / 8);      // 8 for 70 bits
    for (int i = 0; i < full_bytes; ++i) d[i] &= (uint8_t)~s[i];

    // last byte: mask off padding bits (2 LSBs for 70-bit glyphs)
    if (full_bytes < row_bytes) {
      uint8_t b = s[full_bytes] & last_mask;          // zero-out padding bits
      d[full_bytes] &= (uint8_t)~b;
    }
  }

  epd.Display_Partial_Not_refresh(time_paint.GetImage(), x, y, x + stride_bits, y + h);

  time_paint.SetWidth(oldW);
  time_paint.SetHeight(oldH);
}




static const uint8_t* DIG_BITS[10] = {
  gDigit_0, gDigit_1, gDigit_2, gDigit_3, gDigit_4,
  gDigit_5, gDigit_6, gDigit_7, gDigit_8, gDigit_9
};

static inline void DrawBitmapDigitAt(int x, int bandTop, int bandH, int d) {
  if (d < 0 || d > 9) return;
  const Glyph1bpp& g = kDigits[d];
  const int y = bandTop + (bandH - g.height) / 2;  // per-glyph vertical centering
  BlitGlyph1bpp_MSBL(x, y, g.width_bits, g.stride_bits, g.height, g.data);
}

static inline void DrawBitmapColonAt(int x, int y) {
#ifdef gDigit_colon
  BlitBitmapMonoBig(x, y, COLON_W_BMP, COLON_H_BMP, gDigit_colon);
#else
  // Draw little colon into a rect-sized paint buffer, then push.
  const int cw = 12;                        // colon width if no bitmap
  const int ch = DIG_H;
  const int oldW = time_paint.GetWidth();
  const int oldH = time_paint.GetHeight();
  time_paint.SetWidth(cw);
  time_paint.SetHeight(ch);
  time_paint.Clear(UNCOLORED);
  int dot = 8, gap = 10;
  drawColon(time_paint, 0, 0, ch, dot, gap, COLORED);
  epd.Display_Partial_Not_refresh(time_paint.GetImage(), x, y, x + cw, y + ch);
  time_paint.SetWidth(oldW);
  time_paint.SetHeight(oldH);
#endif
}


static inline void DrawSmallTextAt(int x, int y, const char* txt) {
  if (!txt || !*txt) return;

  const int w      = (int)strlen(txt) * Font16.Width;   // 2 * 8px typically = 16px
  const int h      = Font16.Height;                     // ~16px
  const int w8     = (w + 7) & ~7;                      // byte-align width (multiple of 8)

  // Temporarily resize the time_paint buffer to the exact rect
  const int oldW = time_paint.GetWidth();
  const int oldH = time_paint.GetHeight();
  time_paint.SetWidth(w8);
  time_paint.SetHeight(h);

  // Clear to white, draw text at (0,0), then push exactly this rect
  memset(time_paint.GetImage(), 0xFF, (w8/8) * h);      // 1=white
  time_paint.DrawStringAt(0, 0, txt, &Font16, COLORED); // black glyphs

  epd.Display_Partial_Not_refresh(time_paint.GetImage(), x, y, x + w, y + h);

  time_paint.SetWidth(oldW);
  time_paint.SetHeight(oldH);
}



// ---------- helper: filled rounded rectangle (pill) ----------
static void DrawRoundedRectFilled(Paint& p, int x, int y, int w, int h, int r, int color) {
  if (w <= 0 || h <= 0) return;
  if (r < 0) r = 0;
  if (r > w/2) r = w/2;
  if (r > h/2) r = h/2;

  // center block
  if (w > 2*r) p.DrawFilledRectangle(x + r, y, x + w - r - 1, y + h - 1, color);

  // rounded end-caps
  for (int yy = 0; yy < h; ++yy) {
    int dy = (yy < r) ? (r - 1 - yy) : (yy >= h - r ? yy - (h - r) : 0);
    int dx = r ? (int)floorf(sqrtf((float)r * r - (float)dy * dy)) : 0;
    int xL = x + r - dx;
    int xR = x + w - r + dx - 1;
    if (xL <= xR) p.DrawFilledRectangle(xL, y + yy, xR, y + yy, color);
  }
}

// ------------------------------------------------------------
//                    drawNoAircraftScreen()
// ------------------------------------------------------------
void drawNoAircraftScreen(time_t timestamp) {
  ScopedDispLock _;
  ensurePartialPrimed();

  // ----- build date/time -----
  time_t zoned = timestamp + (TZ_minutes * 60);
  struct tm* ti = gmtime(&zoned);

  const bool is24 = USE_24H;
  int  hour24 = ti->tm_hour;
  int  min    = ti->tm_min;

  static bool firstDraw = true;
  static int  lastMin = -1, lastHour24 = -1;
  bool timeChanged = firstDraw || (min != lastMin) || (hour24 != lastHour24);
  if (!timeChanged) return;

  int dispHour = hour24;
  char ampm[3] = "";
  if (!is24) {
    bool pm = (hour24 >= 12);
    int h = hour24 % 12; if (h == 0) h = 12;
    dispHour = h;
    ampm[0] = pm ? 'P' : 'A'; ampm[1] = 'M'; ampm[2] = '\0';
  }

  int hT = dispHour / 10, hO = dispHour % 10;
  int mT = min / 10,      mO = min % 10;

  const int D_H       = kTimeBandH;
  const int GAP_DIG   = 12;
  const int AMPM_GAP  = 10;

  bool twoDigitHour    = is24 ? true : (dispHour >= 10);
  const int GAP_COLON_L = 10;
  const int GAP_COLON_R = twoDigitHour ? 10 : 0 + 10;
  bool hideLeadingZero = (!is24 && !twoDigitHour);

  // ---------- helpers ----------
  auto clearRectWhite = [&](int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) return;
    const int w8 = (w + 7) & ~7;
    const int oldW = time_paint.GetWidth();
    const int oldH = time_paint.GetHeight();
    time_paint.SetWidth(w8);
    time_paint.SetHeight(h);
    memset(time_paint.GetImage(), 0xFF, (w8/8) * h);  // 1=white
    epd.Display_Partial_Not_refresh(time_paint.GetImage(), x, y, x + w, y + h);
    time_paint.SetWidth(oldW);
    time_paint.SetHeight(oldH);
  };

  auto blitGlyphMasked = [&](int x, int y, const Glyph1bpp& g) {
    if (!g.data || g.width_bits <= 0 || g.height <= 0 || (g.stride_bits & 7)) return;
    const int row_bytes = g.stride_bits / 8;

    const int oldW = time_paint.GetWidth();
    const int oldH = time_paint.GetHeight();
    time_paint.SetWidth(g.stride_bits);
    time_paint.SetHeight(g.height);

    uint8_t* dst = time_paint.GetImage();
    memset(dst, 0xFF, row_bytes * g.height);   // white

    const int full_bytes = (g.width_bits / 8);
    const int rem        = (g.width_bits & 7);
    const uint8_t last_mask = rem ? (uint8_t)(0xFF << (8 - rem)) : 0xFF;

    for (int row = 0; row < g.height; ++row) {
      const uint8_t* s = g.data + row * row_bytes;
      uint8_t*       d = dst     + row * row_bytes;
      for (int i = 0; i < full_bytes; ++i) d[i] &= (uint8_t)~s[i];   // 1->black
      if (full_bytes < row_bytes) {
        uint8_t b = s[full_bytes] & last_mask;
        d[full_bytes] &= (uint8_t)~b;
      }
    }

    epd.Display_Partial_Not_refresh(time_paint.GetImage(), x, y,
                                    x + g.stride_bits, y + g.height);
    time_paint.SetWidth(oldW);
    time_paint.SetHeight(oldH);
  };

  auto drawDigitAt = [&](int x, int bandTop, int bandH, int d) -> int {
    if (d < 0 || d > 9) return 0;
    const Glyph1bpp& g = kDigits[d];
    const int visibleW = (int)kDigitVisibleWidthBits; // e.g. 70
    const int clearW   = ((visibleW + 7) & ~7);
    clearRectWhite(x, bandTop, clearW, bandH);
    const int y = bandTop + (bandH - g.height) / 2;
    blitGlyphMasked(x, y, g);
    return visibleW;
  };

  auto drawText24At = [&](int x, int y, const char* txt) {
    if (!txt || !*txt) return;
    const int fw = Font24.Width;
    const int fh = Font24.Height;
    const int w  = (int)strlen(txt) * fw;
    const int w8 = (w + 7) & ~7;
    const int oldW = time_paint.GetWidth();
    const int oldH = time_paint.GetHeight();
    time_paint.SetWidth(w8);
    time_paint.SetHeight(fh);
    memset(time_paint.GetImage(), 0xFF, (w8/8) * fh);
    time_paint.DrawStringAt(0, 0, txt, &Font24, COLORED);
    epd.Display_Partial_Not_refresh(time_paint.GetImage(), x, y, x + w, y + fh);
    time_paint.SetWidth(oldW);
    time_paint.SetHeight(oldH);
  };

  auto DrawRoundedRectFilled = [&](Paint& p, int x, int y, int w, int h, int r, int color) {
    if (w <= 0 || h <= 0) return;
    if (r < 0) r = 0;
    int rmax = std::min(w, h) / 2;
    if (r > rmax) r = rmax;

    if (w - 2*r > 0) p.DrawFilledRectangle(x + r, y, x + w - r - 1, y + h - 1, color);

    int cy = y + h/2;
    for (int dy = -r; dy <= r; ++dy) {
      int dx = (int)floorf(sqrtf((float)r*r - (float)dy*dy));
      int yy = cy + dy;
      p.DrawFilledRectangle(x + r - dx, yy, x + r - 1, yy, color);           // left cap
      p.DrawFilledRectangle(x + w - r,  yy, x + w - r + dx - 1, yy, color);  // right cap
    }
  };

  // ---------- layout bands ----------
  const int headerTop    = kLineH;          // one line down from the very top
  const int headerBottom = headerTop + kLineH;

  const int bandTop = (kScreenH - kTimeBandH) / 2;  // time band
  const int bandBot = bandTop + kTimeBandH;

  // Clear top margin
  pushWhiteBands(0, headerTop);

  // ----- 1) HEADER band (weekday pill + "dd month yyyy") -----
  {
    // Build strings (lowercase weekday & month)
    char wday[24] = {0}, mon[24] = {0}, dayNum[4] = {0}, yearStr[8] = {0};
    strftime(wday,   sizeof(wday), "%A", ti);
    strftime(mon,    sizeof(mon),  "%B", ti);
    strftime(dayNum, sizeof(dayNum), "%d", ti);
    strftime(yearStr,sizeof(yearStr),"%Y", ti);
    for (char* p = wday; *p; ++p) *p = (char)tolower((unsigned char)*p);
    for (char* p = mon;  *p; ++p) *p = (char)tolower((unsigned char)*p);

    // Size header paint to exactly one band (local coords!)
    const int headerW8 = (kScreenW + 7) & ~7;
    paint.SetWidth(headerW8);
    paint.SetHeight(kLineH);
    paint.Clear(UNCOLORED);

    const int fw = Font16.Width;
    const int fh = Font16.Height;

    const int pillPadX = 8;
    const int pillPadY = 4;
    int wdayW = (int)strlen(wday) * fw;
    int pillW = wdayW + pillPadX * 2;
    int pillH = fh + pillPadY * 2;
    if (pillH > kLineH - 2) pillH = kLineH - 2;
    int pillR = pillH / 2;

    const int gap = 12;
    char rest[64];
    snprintf(rest, sizeof(rest), "%s %s %s", dayNum, mon, yearStr);
    int restW = (int)strlen(rest) * fw;

    int totalW = pillW + gap + restW;
    int baseY  = (kLineH - pillH) / 2;           // LOCAL Y
    int startX = (kScreenW - totalW) / 2;

    // pill
    DrawRoundedRectFilled(paint, startX, baseY, pillW, pillH, pillR, COLORED);

    // weekday in white
    int wdayX = startX + pillPadX;
    int wdayY = baseY + (pillH - fh) / 2;
    paint.DrawStringAt(wdayX, wdayY, wday, &Font16, UNCOLORED);

    // rest in black
    int restX = startX + pillW + gap;
    int restY = (kLineH - fh) / 2;               // LOCAL Y
    paint.DrawStringAt(restX, restY, rest, &Font16, COLORED);

    // push header into its screen band
    epd.Display_Partial_Not_refresh(paint.GetImage(), 0, headerTop, kScreenW, headerBottom);
  }

  // ----- 2) CLEAR gap between header and time band -----
  if (bandTop > headerBottom) pushWhiteBands(headerBottom, bandTop);

  // ----- 3) TIME band (bitmap digits + Font24 AM/PM) -----
  {
    const int DIG_W_VIS     = (int)kDigitVisibleWidthBits;
    const bool twoDigitH    = twoDigitHour;
    const int gapL          = GAP_COLON_L;
    const int gapR          = GAP_COLON_R;

#ifdef gDigit_colon
    const int COLON_W = COLON_W_BMP;
    const int COLON_H = COLON_H_BMP;
#else
    const int COLON_W = 12;
    const int COLON_H = kTimeBandH;
#endif

    int digitsW = 0;
    if (!hideLeadingZero) { digitsW += DIG_W_VIS; digitsW += GAP_DIG; }
    digitsW += DIG_W_VIS;                            // hour ones
    digitsW += gapL + COLON_W + gapR;
    digitsW += DIG_W_VIS + GAP_DIG + DIG_W_VIS;      // minute tens + ones

    const int ampmW  = (!is24) ? (AMPM_GAP + (int)strlen(ampm) * Font24.Width) : 0;
    const int totalW = digitsW + ampmW;

    const int startX = (kScreenW - totalW) / 2;

    // HARD CLEAR the whole time band
    clearRectWhite(0, bandTop, kScreenW, kTimeBandH);

    int x = startX;

    if (!hideLeadingZero) { x += drawDigitAt(x, bandTop, kTimeBandH, hT) + GAP_DIG; }
    x += drawDigitAt(x, bandTop, kTimeBandH, hO) + gapL;

    // Colon
#ifdef gDigit_colon
    {
      const int clearW = ((COLON_W + 7) & ~7);
      clearRectWhite(x, bandTop, clearW, kTimeBandH);

      const Glyph1bpp colGlyph{
        gDigit_colon,
        (uint16_t)COLON_W,
        (uint16_t)COLON_H,
        (uint16_t)((COLON_W + 7) & ~7)
      };
      const int y = bandTop + (kTimeBandH - COLON_H) / 2;
      blitGlyphMasked(x, y, colGlyph);
    }
#else
    {
      const int clearW = ((COLON_W + 7) & ~7);
      clearRectWhite(x, bandTop, clearW, kTimeBandH);

      const int oldW = time_paint.GetWidth();
      const int oldH = time_paint.GetHeight();
      time_paint.SetWidth(clearW);
      time_paint.SetHeight(COLON_H);
      time_paint.Clear(UNCOLORED);

      int dot = 8, gap = 10;
      drawColon(time_paint, 0, 0, COLON_H, dot, gap, COLORED);

      const int y = bandTop + (kTimeBandH - COLON_H) / 2;
      epd.Display_Partial_Not_refresh(time_paint.GetImage(), x, y, x + COLON_W, y + COLON_H);

      time_paint.SetWidth(oldW);
      time_paint.SetHeight(oldH);
    }
#endif

    x += COLON_W + gapR;
    x += drawDigitAt(x, bandTop, kTimeBandH, mT) + GAP_DIG;
    x += drawDigitAt(x, bandTop, kTimeBandH, mO);

    // AM/PM with Font24 (text, not bitmap)
    if (!is24) {
      const int fh24   = Font24.Height;
      const int ampmY  = bandTop + (kTimeBandH - fh24) / 2;
      drawText24At(x + AMPM_GAP, ampmY, ampm);
    }
  }

  // ----- 4) CLEAR gap & footer -----
// ----- 4) CLEAR gap & footer (pill outline + lifted) -----
{
  // How far to lift the footer band toward the clock
  const int LIFT_Y = 16;  // tweak to taste

  // Compute the new footer band top
  const int footerBandH = kLineH;                   // keep same band height
  const int footerY     = kScreenH - footerBandH - LIFT_Y;
  const int gapTop      = std::max(bandBot, 0);
  if (footerY > gapTop) pushWhiteBands(gapTop, footerY);   // clear gap above footer band

  // Message and sizing
  const char* footer = "NO AIRCRAFT NEARBY";
  const int fw = Font24.Width;
  const int fh = Font24.Height;

  const int textW = (int)strlen(footer) * fw;
  const int padX  = 16;   // pill left/right padding
  const int padY  = 6;    // pill top/bottom padding
  const int pillW = textW + padX * 2;
  const int pillH = fh + padY * 2;
  const int pillR = pillH / 2;     // full "capsule" ends
  const int stroke = 2;            // border thickness

  // Prepare footer paint buffer (local coords 0..footerBandH-1)
  const int footerW8 = (kScreenW + 7) & ~7;
  paint.SetWidth(footerW8);
  paint.SetHeight(footerBandH);
  paint.Clear(UNCOLORED);  // white

  // Horizontal centering
  const int pillX = (kScreenW - pillW) / 2;
  const int pillY = (footerBandH - pillH) / 2;

  // Helper: pill stroke (outline) built from filled + inset erase
  auto DrawRoundedRectPillStroke = [&](Paint& p, int x, int y, int w, int h, int r, int t, int color) {
    if (w <= 0 || h <= 0) return;
    if (t <= 0) t = 1;
    int rmax = std::min(w, h) / 2;
    if (r > rmax) r = rmax;
    // draw solid
    DrawRoundedRectFilled(p, x, y, w, h, r, color);
    // carve interior to create stroke
    int ix = x + t, iy = y + t, iw = w - 2*t, ih = h - 2*t, ir = std::max(0, r - t);
    if (iw > 0 && ih > 0)
      DrawRoundedRectFilled(p, ix, iy, iw, ih, ir, UNCOLORED);
  };

  // Draw pill outline
  //DrawRoundedRectPillStroke(paint, pillX, pillY, pillW, pillH, pillR, stroke, COLORED);

  // Draw centered text in black
  const int textX = pillX + padX;
  const int textY = pillY + (pillH - fh) / 2;
  paint.DrawStringAt(textX, textY, footer, &Font24, COLORED);

  // Push the footer band at its lifted Y
  epd.Display_Partial_Not_refresh(paint.GetImage(), 0, footerY, kScreenW, footerY + footerBandH);
}


  // 5) refresh
  epd.TurnOnDisplay_Partial();

  firstDraw  = false;
  lastMin    = min;
  lastHour24 = hour24;
}


// ---- Header drawing helpers ----
static inline void DrawVLine(Paint& p, int x, int y1, int y2, int thickness, int color) {
  if (y2 < y1) std::swap(y1, y2);
  for (int t = 0; t < thickness; ++t) p.DrawFilledRectangle(x + t, y1, x + t, y2, color);
}

static inline void DrawHLine(Paint& p, int x1, int x2, int y, int thickness, int color) {
  if (x2 < x1) std::swap(x1, x2);
  for (int t = 0; t < thickness; ++t) p.DrawFilledRectangle(x1, y + t, x2, y + t, color);
}

static inline void plotTL(Paint& p, int cx, int cy, int x, int y, int color) {
  p.DrawAbsolutePixel(cx - x, cy - y, color);
  p.DrawAbsolutePixel(cx - y, cy - x, color);
}
static inline void plotTR(Paint& p, int cx, int cy, int x, int y, int color) {
  p.DrawAbsolutePixel(cx + x, cy - y, color);
  p.DrawAbsolutePixel(cx + y, cy - x, color);
}
static inline void plotBL(Paint& p, int cx, int cy, int x, int y, int color) {
  p.DrawAbsolutePixel(cx - x, cy + y, color);
  p.DrawAbsolutePixel(cx - y, cy + x, color);
}
static inline void plotBR(Paint& p, int cx, int cy, int x, int y, int color) {
  p.DrawAbsolutePixel(cx + x, cy + y, color);
  p.DrawAbsolutePixel(cx + y, cy + x, color);
}

static void drawQuarterCircleOutline(Paint& p, int cx, int cy, int r, int corner, int color) {
  if (r <= 0) return;
  int x = 0, y = r;
  int d = 3 - 2 * r;
  while (x <= y) {
    switch (corner) {
      case 0: plotTL(p, cx, cy, x, y, color); break;
      case 1: plotTR(p, cx, cy, x, y, color); break;
      case 2: plotBL(p, cx, cy, x, y, color); break;
      case 3: plotBR(p, cx, cy, x, y, color); break;
    }
    if (d < 0) d += 4 * x + 6;
    else { d += 4 * (x - y) + 10; --y; }
    ++x;
  }
}

static void DrawRoundedRectOutline(Paint& p, int x, int y, int w, int h,
                                   int r, int thickness, int color) {
  if (w <= 0 || h <= 0 || thickness <= 0) return;
  if (r < 0) r = 0;
  int rmax = (w < h ? w : h) / 2;
  if (r > rmax) r = rmax;

  for (int t = 0; t < thickness; ++t) {
    int xi = x + t, yi = y + t;
    int wi = w - 2 * t, hi = h - 2 * t;
    if (wi <= 0 || hi <= 0) break;

    int rr = r - t; if (rr < 0) rr = 0;
    int left = xi, right = xi + wi - 1, top = yi, bottom = yi + hi - 1;

    if (wi > 0) {
      DrawHLine(p, left + rr, right - rr, top,    1, color);
      DrawHLine(p, left + rr, right - rr, bottom, 1, color);
    }
    if (hi > 0) {
      DrawVLine(p, left,  top + rr, bottom - rr, 1, color);
      DrawVLine(p, right, top + rr, bottom - rr, 1, color);
    }

    if (rr > 0) {
      int cxL = left  + rr, cxR = right - rr;
      int cyT = top   + rr, cyB = bottom - rr;
      drawQuarterCircleOutline(p, cxL, cyT, rr, 0, color);
      drawQuarterCircleOutline(p, cxR, cyT, rr, 1, color);
      drawQuarterCircleOutline(p, cxL, cyB, rr, 2, color);
      drawQuarterCircleOutline(p, cxR, cyB, rr, 3, color);
    }
  }
}

// Parse "YYYY-MM-DD | HH:MM:SS" or with AM/PM
static void ParseHeaderParts(const char* src,
                             char outY[5], char outM[3], char outD[3],
                             char outTime[9], bool& hasAMPM, char outAMPM[3]) {
  outY[0]=outM[0]=outD[0]=outTime[0]=outAMPM[0]='\0';
  hasAMPM = false;
  if (!src || !src[0]) return;

  if (strlen(src) >= 10 && isdigit((unsigned char)src[0])) {
    memcpy(outY, src+0, 4); outY[4]='\0';
    memcpy(outM, src+5, 2); outM[2]='\0';
    memcpy(outD, src+8, 2); outD[2]='\0';
  }
  const char* bar = strchr(src, '|');
  if (bar) {
    bar++;
    while (*bar==' '){ ++bar; }
    if (strlen(bar) >= 8 && isdigit((unsigned char)bar[0])) {
      memcpy(outTime, bar, 8);
      outTime[8]='\0';
      const char* sp = bar + 8;
      while (*sp==' ') ++sp;
      if ((sp[0]=='A' || sp[0]=='P') && (sp[1]=='M')) {
        outAMPM[0]=sp[0]; outAMPM[1]='M'; outAMPM[2]='\0';
        hasAMPM = true;
      }
    }
  }
}

// Blit a 1-bit mono bitmap (MSB-left) using the global 'paint' buffer.
// w must be multiple of 8; '1' bits are drawn as BLACK (COLORED).
static void BlitBitmapMono16(int x, int y, int w, int h, const uint8_t* bitsPROGMEM) {
  if (!bitsPROGMEM || w <= 0 || h <= 0 || (w & 7)) return;
  const int BYTES = (w*h)/8;
  static uint8_t scratch[512]; // 16x16 needs 32 bytes; 512 is plenty
  if (BYTES > (int)sizeof(scratch)) return;

  for (int row=0; row<h; ++row) {
    const uint8_t* src = bitsPROGMEM + row*(w/8);
    uint8_t*       dst = scratch      + row*(w/8);
    memset(dst, 0xFF, w/8);         // white
    for (int i=0;i<w/8;i++) dst[i] &= (uint8_t)~src[i]; // 1->black
  }

  int ow = paint.GetWidth(), oh = paint.GetHeight();
  paint.SetWidth(w);
  paint.SetHeight(h);
  memcpy(paint.GetImage(), scratch, BYTES);

  epd.Display_Partial_Not_refresh(paint.GetImage(), x, y, x+w, y+h);

  paint.SetWidth(ow);
  paint.SetHeight(oh);
}

// Tomohiko Sakamoto’s algorithm: 0=Sun..6=Sat
// 0=Sun..6=Sat (Tomohiko Sakamoto’s algorithm)
static int dowFromYMD(int y, int m, int d) {
  static int t[] = {0,3,2,5,0,3,5,1,4,6,2,4};
  if (m < 3) y -= 1;
  return (y + y/4 - y/100 + y/400 + t[m-1] + d) % 7; // 0..6
}

// Draw a filled rounded pill (height ~ font height)
// Seamless filled "pill" (rounded rectangle) — draws horizontal spans across the full width.
// No center seam, no underlines.
// x,y: top-left; w: width; h: height (h>=2), color: COLORED or UNCOLORED
static void DrawFilledPill(Paint& p, int x, int y, int w, int h, int color) {
  if (w <= 0 || h <= 1) return;
  int r = h / 2;                       // radius from height
  if (2 * r > h) r = h / 2;
  if (2 * r > w) r = w / 2;            // clamp for very small widths

  int left   = x;
  int right  = x + w - 1;
  int top    = y;
  int bottom = y + h - 1;

  // 1) Middle rectangle (no arcs)
  if (right - left + 1 > 0 && r > 0) {
    p.DrawFilledRectangle(left + r, top, right - r, bottom, color);
  } else {
    // If w is too small for side arcs, just fill the whole box
    p.DrawFilledRectangle(left, top, right, bottom, color);
    return;
  }

  // 2) Left & right semicircles (horizontal spans — no seam!)
  //    We draw each scanline from the arc edge all the way across to the opposite arc edge.
  //    That guarantees a single solid fill per row.
  for (int dy = -r; dy <= r; ++dy) {
    int yy = y + r + dy;                          // current scanline
    int dx = 0;
    // If r*r - dy*dy is negative due to integer rounding, clamp to 0
    int rr_minus_dy2 = r*r - dy*dy;
    if (rr_minus_dy2 > 0) {
      // integer sqrt (fast + exact enough for small r)
      dx = (int)floorf(sqrtf((float)rr_minus_dy2));
    }
    int xl = left  + r - dx;                      // left arc x
    int xr = right - r + dx;                      // right arc x
    if (xl > xr) std::swap(xl, xr);
    p.DrawFilledRectangle(xl, yy, xr, yy, color); // one continuous span — no center join
  }
}

// ========== FAST overlay helpers (paint-only; single EPD push) ==========
static inline void PaintDottedH(Paint& p, int x, int y, int w, int thickness, int onLen, int offLen) {
  if (w <= 0 || thickness <= 0) return;
  for (int xi = 0; xi < w; ) {
    int on = std::min(onLen, w - xi);
    p.DrawFilledRectangle(x + xi, y, x + xi + on - 1, y + thickness - 1, COLORED);
    xi += on + offLen;
  }
}

static inline void PaintDottedV(Paint& p, int x, int y, int h, int thickness, int onLen, int offLen) {
  if (h <= 0 || thickness <= 0) return;
  for (int yi = 0; yi < h; ) {
    int on = std::min(onLen, h - yi);
    p.DrawFilledRectangle(x, y + yi, x + thickness - 1, y + yi + on - 1, COLORED);
    yi += on + offLen;
  }
}

// Dashed diagonal drawn entirely into 'p' (RAM). Thickness = 1 px.
static inline void PaintDashedDiag(Paint& p, int x0, int y0, int x1, int y1, int onLen, int offLen) {
  int dx = std::abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
  int dy = -std::abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
  int err = dx + dy;

  int dashRemain = onLen;  // remaining pixels in current ON/OFF segment
  bool on = true;

  auto stepDash = [&](){
    if (--dashRemain <= 0) {
      on = !on;
      dashRemain = on ? onLen : offLen;
    }
  };

  while (true) {
    if (on) p.DrawFilledRectangle(x0, y0, x0, y0, COLORED); // 1px
    stepDash();
    if (x0 == x1 && y0 == y1) break;
    int e2 = 2 * err;
    if (e2 >= dy) { err += dy; x0 += sx; }
    if (e2 <= dx) { err += dx; y0 += sy; }
  }
}

// Draws dotted square + dashed X into a temporary paint buffer, then single push.
// - Diagonals start at the exact corners
// - Adds a small solid core at the intersection for a proper cross
// Draws dotted square + dashed X into a temporary paint buffer, then single push.
// - Diagonals start at the exact corners
// - No center square (optional 1px dot can be enabled via CROSS_CENTER_DOT)
static void drawEmptySpaceOverlay_Fast(int emptyTopY, int emptyBottomY) {
  // Same dotted style as rows
  const int onLen = 4, offLen = 3, thickness = 1;

  // Keep off row borders a bit
  const int marginX = 12;
  const int marginY = 8;

  // Height of the empty strip
  int overlayH = emptyBottomY - emptyTopY;
  if (overlayH <= 0) return;
  if (overlayH < 36) return;  // too small to look nice

  // Prepare 'paint' as an overlay buffer the size of the empty strip
  const int overlayW8 = (kScreenW + 7) & ~7;
  int oldW = paint.GetWidth(), oldH = paint.GetHeight();
  paint.SetWidth(overlayW8);
  paint.SetHeight(overlayH);
  paint.Clear(UNCOLORED);

  // Local coords inside this buffer
  const int leftX  = marginX;
  const int rightX = kScreenW - marginX - 1;
  const int topY   = marginY;
  const int botY   = overlayH - marginY - 1;

  // Sane room check
  if (rightX - leftX < 20 || botY - topY < 20) {
    epd.Display_Partial_Not_refresh(paint.GetImage(), 0, emptyTopY, kScreenW, emptyBottomY);
    paint.SetWidth(oldW); paint.SetHeight(oldH);
    return;
  }

  // -------- Dotted square outline (square corners) --------
  // Top / Bottom
  PaintDottedH(paint, leftX,  topY,  rightX - leftX + 1, thickness, onLen, offLen);
  PaintDottedH(paint, leftX,  botY,  rightX - leftX + 1, thickness, onLen, offLen);
  // Left / Right
  PaintDottedV(paint, leftX,  topY,  botY - topY + 1,    thickness, onLen, offLen);
  PaintDottedV(paint, rightX, topY,  botY - topY + 1,    thickness, onLen, offLen);

  // -------- Dashed 'X' starting at the EXACT corners --------
  // TL -> BR
  PaintDashedDiag(paint, leftX,  topY,  rightX, botY, onLen, offLen);
  // BL -> TR
  PaintDashedDiag(paint, leftX,  botY,  rightX, topY, onLen, offLen);

  // Optional: 1px center dot to guarantee a clean "meet" (OFF by default)
  #define CROSS_CENTER_DOT 0
  #if CROSS_CENTER_DOT
    {
      const int cx = (leftX + rightX) / 2;
      const int cy = (topY  + botY ) / 2;
      paint.DrawFilledRectangle(cx, cy, cx, cy, COLORED);
    }
  #endif

  // Single partial push for the whole empty region
  epd.Display_Partial_Not_refresh(paint.GetImage(), 0, emptyTopY, kScreenW, emptyBottomY);

  // restore paint
  paint.SetWidth(oldW);
  paint.SetHeight(oldH);
}





// -------- LIVE (batched partial, single refresh) --------
void drawAircraftInfoToDisplay_Partial(const char* timeStr, int totalAircraftFromAPI) {
  if (sMode == HOLD_MODE) return;
  ensurePartialPrimed();

  RowView* rows = s_rows_live;
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

  // cosmetics
  const int kSepThickness = 1;
  const int kDotThickness = 1;
  const int kDotOn  = 4;
  const int kDotOff = 3;

  auto drawSolidHLine = [&](int y0, int thickness) {
    for (int t = 0; t < thickness; ++t)
      paint.DrawFilledRectangle(0, y0 + t, kScreenW - 1, y0 + t, COLORED);
  };
  auto drawDottedVLine = [&](int x0, int thickness, int onLen, int offLen) {
    for (int t = 0; t < thickness; ++t) {
      for (int y = 0; y < kLineH; y += (onLen + offLen)) {
        int y1 = y, y2 = y + onLen - 1;
        if (y2 >= kLineH) y2 = kLineH - 1;
        paint.DrawFilledRectangle(x0 + t, y1, x0 + t, y2, COLORED);
      }
    }
  };

  int y = 0;
  int linesUsed = 0;

// =========================
// Header ( [DoW pill] DD Mon  |  ⏰ HH:MM [AM/PM]  |  ✈ AVL n )
// =========================
{
  const char* src = (timeStr && timeStr[0]) ? timeStr : sLastTimeStr;

  // Parse: YYYY-MM-DD | HH:MM:SS [AM/PM]
  char yy[5], mm[3], dd[3], hhmmss[9], ampm[3];
  bool haveAMPM = false;
  ParseHeaderParts(src, yy, mm, dd, hhmmss, haveAMPM, ampm);

  // Use HH:MM only
  char hhmm[6] = "--:--";
  if (isdigit((unsigned char)hhmmss[0]) && isdigit((unsigned char)hhmmss[1]) &&
      hhmmss[2] == ':' &&
      isdigit((unsigned char)hhmmss[3]) && isdigit((unsigned char)hhmmss[4])) {
    memcpy(hhmm, hhmmss, 5); hhmm[5] = '\0';
  }

  // Month / weekday
  static const char* kMon[13] = {"","Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};
  int mnum = (mm[0] && mm[1]) ? ((mm[0]-'0')*10 + (mm[1]-'0')) : 0;
  const char* mon3 = (mnum>=1 && mnum<=12) ? kMon[mnum] : "--";

  static const char* kDOW2[7] = {"Su","Mo","Tu","We","Th","Fr","Sa"};
  int yN = atoi(yy), mN = mnum, dN = atoi(dd);
  int dow = (yN>0 && mN>=1 && mN<=12 && dN>=1 && dN<=31) ? dowFromYMD(yN, mN, dN) : 0;
  const char* dow2 = kDOW2[(dow+7)%7];

  char dateLabel[16];
  snprintf(dateLabel, sizeof(dateLabel), "%s %s", dd[0] ? dd : "--", mon3);

  int totalToShow = (totalAircraftFromAPI > 0) ? totalAircraftFromAPI : sTotalActive;
  char totalLabel[24]; snprintf(totalLabel, sizeof(totalLabel), "AVL %d", totalToShow);

  // Capsule + “safe box” that keeps content off the outline
  const int OUT_X = 2, OUT_Y = 1;                 // outline origin
  const int OUT_W = kScreenW - 4, OUT_H = kLineH - 2;
  const int STROKE = 2;                            // we draw 2px outline
  const int MARGIN = 2;                            // inner breathing room

  const int SAFE_X = OUT_X + STROKE + MARGIN;
  const int SAFE_Y = OUT_Y + STROKE + MARGIN;
  const int SAFE_W = OUT_W - 2*(STROKE + MARGIN);
  const int SAFE_H = OUT_H - 2*(STROKE + MARGIN);

  // Center/right icon metrics
  const int iconW = 16, iconH = 16;
  const int gapIconText = 6;

  // A single baseline for all header text (vertically centered in SAFE box)
  int yText = SAFE_Y + (SAFE_H - Font16.Height)/2 + 2;

  // Draw capsule outline
  paint.Clear(UNCOLORED);
  DrawRoundedRectOutline(paint, OUT_X, OUT_Y, OUT_W, OUT_H, 8, STROKE, COLORED);

  // -------- LEFT: [DoW pill] + gap + "DD Mon"  --------
  const int pillPadX = 6;
  const int pillPadY = 2;

  int pillTextW = 2 * Font16.Width;                // "Mo" etc.
  int pillH     = Font16.Height + 2*pillPadY;
  if (pillH > SAFE_H) pillH = SAFE_H;              // never touch the capsule
  int pillW     = pillTextW + 2*pillPadX;

  // Pill centered vertically in SAFE box (so it does not touch borders)
  int pillX = SAFE_X;                               // snug to the safe left
  int pillY = SAFE_Y + (SAFE_H - pillH)/2;

  DrawFilledPill(paint, pillX, pillY, pillW, pillH, COLORED);
  int pillTextY = pillY + (pillH - Font16.Height)/2 + 2;
  paint.DrawStringAt(pillX + pillPadX, pillTextY, dow2, &Font16, UNCOLORED);

  const int gapPillToDate = 8;
  int xDate = pillX + pillW + gapPillToDate;
  paint.DrawStringAt(xDate, yText, dateLabel, &Font16, COLORED);

  // -------- CENTER: ⏰ + HH:MM [AM/PM] (as one centered block in SAFE) --------
  int timeW = (int)strlen(hhmm) * Font16.Width + (haveAMPM ? (Font16.Width + 2*Font16.Width) : 0);

  // block width: icon + gap + time text (incl. AM/PM if present)
  int centerBlockW = iconW + gapIconText + ((int)strlen(hhmm) * Font16.Width) + (haveAMPM ? (Font16.Width + 2*Font16.Width) : 0);
  int xC = SAFE_X + (SAFE_W - centerBlockW)/2;
  int xC_text = xC + iconW + gapIconText;

  paint.DrawStringAt(xC_text, yText, hhmm, &Font16, COLORED);
  if (haveAMPM) {
    paint.DrawStringAt(xC_text + 5*Font16.Width, yText, " ",  &Font16, COLORED);
    paint.DrawStringAt(xC_text + 6*Font16.Width, yText, ampm, &Font16, COLORED);
  }

  // -------- RIGHT: ✈ + "AVL n" --------
  int totalW = (int)strlen(totalLabel) * Font16.Width;
  int rightBlockW = iconW + gapIconText + totalW;
  int xR = SAFE_X + SAFE_W - rightBlockW;
  int xR_text = xR + iconW + gapIconText;
  paint.DrawStringAt(xR_text, yText, totalLabel, &Font16, COLORED);

  // Push the header band
  epd.Display_Partial_Not_refresh(paint.GetImage(), 0, 0, kScreenW, kLineH);

  // Overlay icons (center/right only)
  int iconY = SAFE_Y + (SAFE_H - iconH)/2;
  BlitBitmapMono16(xC, iconY, iconW, iconH, ICON_CLK_16);
  BlitBitmapMono16(xR, iconY, iconW, iconH, ICON_PLN_16);

  // advance rows start
  y = kLineH;
  linesUsed++;
}

  // ---- Rows ----
  int shown = 0;
  for (int i = 0; i < active && shown < maxShown; i++) {
    const auto& r = rows[i];

    // Callsign or ICAO (fixed 7 chars)
    char callsign[9];
    if (r.callsign[0] == '\0') { strncpy(callsign, r.icao24, sizeof(callsign)); callsign[sizeof(callsign)-1] = '\0'; }
    else                       { strncpy(callsign, r.callsign, sizeof(callsign)); callsign[sizeof(callsign)-1] = '\0'; }
    char csPadded[8]; snprintf(csPadded, sizeof(csPadded), "%-7.7s", callsign);

    // Distance "xxxx.xkm"
    char distStr[10]; snprintf(distStr, sizeof(distStr), "%4.1fkm", r.distance);

    // Direction "DDDXY"
    int  bInt = (int)r.bearing; if (bInt < 0) bInt += 360; if (bInt > 359) bInt -= 360;
    char brg[4];  snprintf(brg, sizeof(brg), "%3d", bInt);
    char cd[2];   compass2(r.bearing, cd);
    char dirStr[6]; snprintf(dirStr, sizeof(dirStr), "%s%s", brg, cd);

    // Model band
    paint.Clear(UNCOLORED);
    if (shown > 0) drawSolidHLine(0, kSepThickness);
    paint.DrawStringAt(5, 5, r.model, &Font16, COLORED);

    // dotted underline
    for (int t = 0; t < kDotThickness; ++t) {
      for (int x = 0; x < kScreenW; x += (kDotOn + kDotOff)) {
        int x1 = x, x2 = x + kDotOn - 1; if (x2 >= kScreenW) x2 = kScreenW - 1;
        paint.DrawFilledRectangle(x1, kLineH - 1 + t, x2, kLineH - 1 + t, COLORED);
      }
    }
    epd.Display_Partial_Not_refresh(paint.GetImage(), 0, y, kScreenW, y + kLineH);
    y += kLineH; linesUsed++;

    // Info band
    paint.Clear(UNCOLORED);

    const int fw = Font16.Width;
    const int left = 5;
    const int pad  = 4;
    const int spaceAfterDot = fw;
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

    // Country fitted into remaining space
    const int rightPad = 5;
    int maxCountryPx = kScreenW - rightPad - x4_text;
    int maxCountryCh = (maxCountryPx > 0) ? (maxCountryPx / fw) : 0;
    char countryBuf[32];
    shortenCountryForWidth(r.country, countryBuf, sizeof(countryBuf), maxCountryCh);

    paint.DrawStringAt(x1_text, 5, csPadded,   &Font16, COLORED);
    paint.DrawStringAt(x2_text, 5, distStr,    &Font16, COLORED);
    paint.DrawStringAt(x3_text, 5, dirStr,     &Font16, COLORED);
    paint.DrawStringAt(x4_text, 5, countryBuf, &Font16, COLORED);

    if (x1_line < kScreenW) drawDottedVLine(x1_line, kDotThickness, kDotOn, kDotOff);
    if (x2_line < kScreenW) drawDottedVLine(x2_line, kDotThickness, kDotOn, kDotOff);
    if (x3_line < kScreenW) drawDottedVLine(x3_line, kDotThickness, kDotOn, kDotOff);

    for (int t = 0; t < kSepThickness; ++t)
      paint.DrawFilledRectangle(0, kLineH - kSepThickness + t, kScreenW - 1, kLineH - kSepThickness + t, COLORED);

    epd.Display_Partial_Not_refresh(paint.GetImage(), 0, y, kScreenW, y + kLineH);
    y += kLineH; linesUsed++;

    shown++;
    yield();
    if (gHoldRequested) return;
  }


  // Clear leftovers (blank any unused 27px bands) with one push per band
  for (int i = linesUsed; i < maxLines; i++) {
    paint.Clear(UNCOLORED);
    epd.Display_Partial_Not_refresh(
        paint.GetImage(),
        0,
        i * kLineH,
        kScreenW,
        (i + 1) * kLineH
    );
    yield();
    if (gHoldRequested) return;
  }

  // Draw fast dotted box + dashed X in the remaining empty strip (single push)
  {
    int emptyTopY    = linesUsed * kLineH;
    int emptyBottomY = kScreenH;
    if (emptyBottomY > emptyTopY) {
      drawEmptySpaceOverlay_Fast(emptyTopY, emptyBottomY);
    }
  }

  // Final partial refresh
  if (!gHoldRequested) epd.TurnOnDisplay_Partial();

}


void drawHoldPagePartial() {
  // ===== snapshot & sort =====
  RowView* rows = s_rows_hold;
  int active = snapshotActiveRows(rows, MAX_CACHE_SIZE);
  std::sort(rows, rows + active, [](const RowView& a, const RowView& b){
    return a.distance < b.distance;
  });
  recalcPagingFromActive(active);

  ensurePartialPrimed();

  const int maxShown = 5;
  const int start    = (sCurrentPage - 1) * maxShown;

  // ===== build HH:MM from sLastTimeStr (fallback to local) =====
  char hhmm[6] = "--:--";
  if (sLastTimeStr[0]) {
    const char* p = strchr(sLastTimeStr, '|');
    if (p) {
      p++; while (*p && !isdigit((unsigned char)*p)) p++;
      int H=0, M=0;
      if (sscanf(p, "%2d:%2d", &H, &M) == 2) {
        if (H < 0) H = 0; if (H > 23) H %= 24;
        if (M < 0) M = 0; if (M > 59) M %= 60;
        snprintf(hhmm, sizeof(hhmm), "%02d:%02d", H, M);
      }
    }
  }
  if (hhmm[0]=='-' && hhmm[1]=='-') {
    time_t now = time(nullptr);
    struct tm ti; localtime_r(&now, &ti);
    snprintf(hhmm, sizeof(hhmm), "%02d:%02d", ti.tm_hour, ti.tm_min);
  }

  // ===== labels =====
  char pageXY[16];  snprintf(pageXY, sizeof(pageXY), "%d/%d", sCurrentPage, sTotalPages);
  char totalN[8];   snprintf(totalN,  sizeof(totalN), "%d",    sTotalActive);

  // ===== metrics (mirror dummy HOLD header exactly) =====
  const int radius = 8;
  const int padX   = 6;
  const int iconW  = 16, iconH = 16;
  const int iconY  = 5;                         // visually centered in 27px band
  const int fw     = Font16.Width;
  const int gapIconText = 6;                    // gap between icon and label

  // match the 1px baseline nudge you use elsewhere
  const int HEADER_TEXT_Y_ADJ = 2;
  const int yText = iconY + ((iconH - Font16.Height) / 2) + HEADER_TEXT_Y_ADJ;

  // LEFT block: ⏸ + HOLD
  const char* leftLabel = "HOLD";
  int leftW    = (int)strlen(leftLabel) * fw;

  // CENTER combined block: [⏰ + HH:MM] + one normal space + [📄 + X/Y]
  int centerW_time = (int)strlen(hhmm)   * fw;
  int centerW_page = (int)strlen(pageXY) * fw;
  int centerBlockW = (iconW + gapIconText + centerW_time)
                   + fw /*single space*/
                   + (iconW + gapIconText + centerW_page);

  // RIGHT block: [💾 + N]
  int rightW_num   = (int)strlen(totalN) * fw;
  int rightBlockW  = iconW + gapIconText + rightW_num;

  // ===== draw header band (outline + text FIRST) =====
  int y = 0;
  paint.Clear(UNCOLORED);
  DrawRoundedRectOutline(paint, 2, 1, kScreenW - 4, kLineH - 2, radius, 2, COLORED);

  // LEFT text
  int xL_icon = padX + 2;
  int xL_text = xL_icon + iconW + gapIconText;
  paint.DrawStringAt(xL_text, yText, leftLabel, &Font16, COLORED);

  // CENTER text (as a single centered block)
  int xC_block = (kScreenW - centerBlockW) / 2;

  int xC1_icon = xC_block;
  int xC1_text = xC1_icon + iconW + gapIconText;
  paint.DrawStringAt(xC1_text, yText, hhmm, &Font16, COLORED);

  // one normal space between the two center groups
  int xC2_icon = xC1_text + centerW_time + fw;
  int xC2_text = xC2_icon + iconW + gapIconText;
  paint.DrawStringAt(xC2_text, yText, pageXY, &Font16, COLORED);

  // RIGHT text
  int xR_icon = kScreenW - padX - 2 - rightBlockW;
  int xR_text = xR_icon + iconW + gapIconText;
  paint.DrawStringAt(xR_text, yText, totalN, &Font16, COLORED);

  // push header band
  epd.Display_Partial_Not_refresh(paint.GetImage(), 0, y, kScreenW, y + kLineH);

  // overlay icons AFTER text (crisp edges) — EXACTLY like dummy
  BlitBitmapMono16(xL_icon, iconY, iconW, iconH, ICON_PAUSE_16); // ⏸
  BlitBitmapMono16(xC1_icon, iconY, iconW, iconH, ICON_CLK_16);  // ⏰
  BlitBitmapMono16(xC2_icon, iconY, iconW, iconH, ICON_PAGE_16); // 📄
  BlitBitmapMono16(xR_icon, iconY, iconW, iconH, ICON_SAVE_16);  // 💾

  y += kLineH;

  // ===== row cosmetics (same as your live/dummy styling) =====
  const int kSepThickness = 1;
  const int kDotThickness = 1;
  const int kDotOn  = 4;
  const int kDotOff = 3;

  auto drawDottedHLine = [&](int y0, int thickness, int onLen, int offLen){
    for (int t = 0; t < thickness; ++t) {
      for (int x = 0; x < kScreenW; x += (onLen + offLen)) {
        int x1 = x, x2 = x + onLen - 1; if (x2 >= kScreenW) x2 = kScreenW - 1;
        paint.DrawFilledRectangle(x1, y0 + t, x2, y0 + t, COLORED);
      }
    }
  };
  auto drawDottedVLine = [&](int x0, int thickness, int onLen, int offLen){
    for (int t = 0; t < thickness; ++t) {
      for (int yy = 0; yy < kLineH; yy += (onLen + offLen)) {
        int y1 = yy, y2 = yy + onLen - 1; if (y2 >= kLineH) y2 = kLineH - 1;
        paint.DrawFilledRectangle(x0 + t, y1, x0 + t, y2, COLORED);
      }
    }
  };

  // ===== columns (match live) =====
  const int left = 5;
  const int pad  = 4;
  const int spaceAfterDot = fw;

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

  // ===== rows =====
  int shown = 0;
  for (int i = start; i < active && shown < maxShown; i++) {
    const auto& r = rows[i];

    // callsign (fallback to ICAO), fixed 7 chars
    char callsign[9];
    if (r.callsign[0] == '\0') { strncpy(callsign, r.icao24, sizeof(callsign)); callsign[sizeof(callsign)-1] = '\0'; }
    else                       { strncpy(callsign, r.callsign, sizeof(callsign)); callsign[sizeof(callsign)-1] = '\0'; }
    char csPadded[8]; snprintf(csPadded, sizeof(csPadded), "%-7.7s", callsign);

    // distance → "%4.1fkm"
    char distStr[10]; snprintf(distStr, sizeof(distStr), "%4.1fkm", r.distance);

    // direction "DDDcc"
    int  bInt = (int)r.bearing; if (bInt < 0) bInt += 360; bInt %= 360;
    char brg[4];  snprintf(brg, sizeof(brg), "%3d", bInt);
    char cd[3];   { static const char t8[8][3]={"N ","NE","E ","SE","S ","SW","W ","NW"};
                    int idx=(int)lroundf((float)bInt/45.0f); if(idx==8) idx=0; cd[0]=t8[idx][0]; cd[1]=t8[idx][1]; cd[2]='\0'; }
    char dirStr[8]; snprintf(dirStr, sizeof(dirStr), "%s%s", brg, cd);

    // country trimmed to remaining width
    const int rightPad = 5;
    int maxCountryPx = kScreenW - rightPad - x4_text;
    int maxCountryCh = (maxCountryPx > 0) ? (maxCountryPx / fw) : 0;
    char countryBuf[32];
    shortenCountryForWidth(r.country, countryBuf, sizeof(countryBuf), maxCountryCh);

    // Model band
    paint.Clear(UNCOLORED);
    if (shown > 0) paint.DrawFilledRectangle(0, 0, kScreenW - 1, 0, COLORED);  // top border
    paint.DrawStringAt(left, 5, r.model, &Font16, COLORED);
    drawDottedHLine(kLineH - 1, kDotThickness, kDotOn, kDotOff);
    epd.Display_Partial_Not_refresh(paint.GetImage(), 0, y, kScreenW, y + kLineH);
    y += kLineH;

    // Info band
    paint.Clear(UNCOLORED);
    if (x1_line < kScreenW) drawDottedVLine(x1_line, kDotThickness, kDotOn, kDotOff);
    if (x2_line < kScreenW) drawDottedVLine(x2_line, kDotThickness, kDotOn, kDotOff);
    if (x3_line < kScreenW) drawDottedVLine(x3_line, kDotThickness, kDotOn, kDotOff);

    paint.DrawStringAt(x1_text, 5, csPadded,   &Font16, COLORED);
    paint.DrawStringAt(x2_text, 5, distStr,    &Font16, COLORED);
    paint.DrawStringAt(x3_text, 5, dirStr,     &Font16, COLORED);
    paint.DrawStringAt(x4_text, 5, countryBuf, &Font16, COLORED);

    paint.DrawFilledRectangle(0, kLineH - 1, kScreenW - 1, kLineH - 1, COLORED);
    epd.Display_Partial_Not_refresh(paint.GetImage(), 0, y, kScreenW, y + kLineH);
    y += kLineH;

    shown++;
    if (gHoldRequested) break;
  }

  // clear any remainder
  while (y < kScreenH) {
    int y2 = y + kLineH; if (y2 > kScreenH) y2 = kScreenH;
    paint.Clear(UNCOLORED);
    epd.Display_Partial_Not_refresh(paint.GetImage(), 0, y, kScreenW, y2);
    y = y2;
  }

  if (!gHoldRequested) epd.TurnOnDisplay_Partial();
}



// -------- Mode & paging API --------
DisplayMode getDisplayMode() { return sMode; }

void setDisplayMode(DisplayMode m) {
  // Guard: don’t enter HOLD when <6 aircraft
  if (m == HOLD_MODE && getActiveCount() < 2) return;

  if (sMode == m) {
    if (m == HOLD_MODE) {
      recalcPagingFromActive(getActiveCount());
      sCurrentPage = std::max(1, std::min(sCurrentPage, sTotalPages));
      ensurePartialPrimed();
      drawHoldPagePartial();
    }
    return;
  }

  sMode = m;

  if (sMode == HOLD_MODE) {
    ensurePartialPrimed();
    recalcPagingFromActive(getActiveCount());
    sCurrentPage = 1;
    drawHoldPagePartial();
  } else {
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



