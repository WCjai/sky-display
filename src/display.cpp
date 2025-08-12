// display.cpp
#include "display.h"
#include "cache.h"

extern volatile bool gHoldRequested; // set from main.cpp ISRs

Epd epd;
unsigned char full_image[400 / 8 * 300];
Paint full_paint(full_image, 400, 300);

unsigned char image[400 / 8 * 28];
Paint paint(image, 400, 28);
int scrollOffset = 0;

static DisplayMode sMode = LIVE_MODE;
DisplayMode getDisplayMode() { return sMode; }
static int sCurrentPage = 1;       // 1-indexed
static int sTotalPages  = 1;       // calculated from active aircraft
static int sTotalActive = 0;       // active rows in cache
static char sLastTimeStr[64] = "";   // last LIVE header timestamp
extern Epd epd;
extern Paint paint;        // used by LIVE partial (if you keep it)
extern Paint full_paint;   // 400x300 full buffer created in setup()


// Count active rows
static int countActiveAircraft() {
  int cnt = 0;
  for (int i = 0; i < MAX_CACHE_SIZE; i++) {
    if (aircraftCache[i].icao24 != "" && aircraftCache[i].distance >= 0) cnt++;
  }
  return cnt;
}

static void recalcPaging() {
  sTotalActive = countActiveAircraft();
  sTotalPages  = max(1, (sTotalActive + 5 - 1) / 5);  // 5 per page
  if (sCurrentPage > sTotalPages) sCurrentPage = sTotalPages;
  if (sCurrentPage < 1) sCurrentPage = 1;
}

void drawStatusScreen(const char* msg) {
    full_paint.Clear(UNCOLORED);
    epd.Init();
    epd.Clear();

    sFONT* font = &Font16;
    int16_t w = strlen(msg) * font->Width;
    int16_t h = font->Height;
    int16_t x = (400 - w) / 2;
    int16_t y = (300 - h) / 2;

    full_paint.DrawStringAt(x, y, msg, font, COLORED);
    epd.Display(full_paint.GetImage());
}

void drawStatusScreenwithline(const char* line1,
                              const char* line2,
                              const char* line3,
                              const char* line4,
                              const char* line5,
                              const char* line6) {
    full_paint.Clear(UNCOLORED);
    epd.Init();
    epd.Clear();

    sFONT* font = &Font16;
    int lineHeight = font->Height + 4;
    int totalLines = 6;
    int startY = (300 - (lineHeight * totalLines)) / 2;

    const char* lines[] = {line1, line2, line3, line4, line5, line6};

    for (int i = 0; i < totalLines; i++) {
        if (strlen(lines[i]) > 0) {
            full_paint.DrawStringAt(10, startY + i * lineHeight, lines[i], font, COLORED);
        }
    }

    epd.Display(full_paint.GetImage());
}

void drawNoAircraftScreen(time_t timestamp) {
    // Apply timezone offset
    time_t zoned = timestamp + (TZ_minutes * 60);
    struct tm* ti = gmtime(&zoned);

    // Date: "Wed 2025-07-10"
    char dateStr[32];
    strftime(dateStr, sizeof(dateStr), "%a %Y-%m-%d", ti);

    // Time string
    char clockStr[16];
    if (USE_24H) {
        strftime(clockStr, sizeof(clockStr), "%H:%M", ti);
    } else {
        strftime(clockStr, sizeof(clockStr), "%I:%M %p", ti);
    }

    const int screenWidth = 400;
    const int screenHeight = 300;
    const int lineHeight = 27;

    int currentY = 0;

    // — 1) Date + Day
    paint.Clear(UNCOLORED);
    int dateX = (screenWidth - strlen(dateStr) * Font16.Width) / 2;
    paint.DrawStringAt(dateX, 5, dateStr, &Font16, COLORED);
    epd.Display_Partial(paint.GetImage(), 0, currentY, screenWidth, currentY + lineHeight);
    currentY += lineHeight;

    // — 2) Time (vertically centered)
    int clockY = (screenHeight - Font24.Height) / 2;
    paint.Clear(UNCOLORED);
    int clockX = (screenWidth - strlen(clockStr) * Font24.Width) / 2;
    //paint.DrawStringAt(clockX, 5, clockStr, &Font24, COLORED);
    paint.DrawStringAt(clockX, 5, clockStr, &Font24, COLORED);
    paint.DrawStringAt(clockX + 1, 5, clockStr, &Font24, COLORED);  // Draw again slightly offset

    epd.Display_Partial(paint.GetImage(), 0, clockY, screenWidth, clockY + lineHeight);

    // — 3) Footer
    const char* footer = "no aircraft nearby";
    int footerY = screenHeight - lineHeight;
    paint.Clear(UNCOLORED);
    int footerX = (screenWidth - strlen(footer) * Font16.Width) / 2;
    paint.DrawStringAt(footerX, 5, footer, &Font16, COLORED);
    epd.Display_Partial(paint.GetImage(), 0, footerY, screenWidth, footerY + lineHeight);
}


// void drawAircraftInfoToDisplay_Partial(const char* timeStr, int totalAircraft) {
//   const int lineHeight = 27;
//   const int maxShownAircraft = 5;
//   const int maxLines = 1 + (maxShownAircraft * 2);
//   int currentLine = 0;
//   int linesUsed = 0;

//   sortAircraftCacheByDistance();

//   char header[64];
//   snprintf(header, sizeof(header), "%s | Total:%d", timeStr, totalAircraft);
//   paint.Clear(UNCOLORED);
//   paint.DrawStringAt(0, 5, header, &Font16, COLORED);
//   //paint.DrawStringAt(1, 5, header, &Font16, COLORED); 
//   epd.Display_Partial(paint.GetImage(), 0, currentLine, 400, currentLine + lineHeight);
//   currentLine += lineHeight;
//   linesUsed++;

//   int shown = 0;
//   for (int i = 0; i < MAX_CACHE_SIZE && shown < maxShownAircraft; i++) {
//     if (aircraftCache[i].icao24 != "" && aircraftCache[i].distance >= 0) {
//       String dir = getCompassDirection(aircraftCache[i].bearing);
//       String model = aircraftCache[i].model;
//       String callsign = aircraftCache[i].callsign;
//       String country = aircraftCache[i].country;
//       if (callsign == "") callsign = aircraftCache[i].icao24;

//       char paddedCallsign[8];
//       snprintf(paddedCallsign, sizeof(paddedCallsign), "%-7s", callsign.c_str());
//       char paddedDistance[7];
//       snprintf(paddedDistance, sizeof(paddedDistance), "%6.2f", aircraftCache[i].distance);
//       int bearingInt = (int)aircraftCache[i].bearing;
//       char paddedBearing[4];
//       snprintf(paddedBearing, sizeof(paddedBearing), "%3d", bearingInt);
//       String compassDir = dir.substring(dir.length() - 2);

//       char infoLine[64];
//       snprintf(infoLine, sizeof(infoLine), "%s%skm %s%s %s",
//               paddedCallsign, paddedDistance, paddedBearing, compassDir.c_str(), country.c_str());

//       bool invertBlock = (shown % 2 == 0);

//       paint.Clear(invertBlock ? COLORED : UNCOLORED);
//       paint.DrawStringAt(5, 5, model.c_str(), &Font16, invertBlock ? UNCOLORED : COLORED);
//       epd.Display_Partial(paint.GetImage(), 0, currentLine, 400, currentLine + lineHeight);
//       currentLine += lineHeight;
//       linesUsed++;

//       paint.Clear(invertBlock ? COLORED : UNCOLORED);
//       paint.DrawStringAt(5, 5, infoLine, &Font16, invertBlock ? UNCOLORED : COLORED);
//       epd.Display_Partial(paint.GetImage(), 0, currentLine, 400, currentLine + lineHeight);
//       currentLine += lineHeight;
//       linesUsed++;

//       shown++;
//     }
//   }

//   for (int i = linesUsed; i < maxLines; i++) {
//     paint.Clear(UNCOLORED);
//     epd.Display_Partial(paint.GetImage(), 0, i * lineHeight, 400, (i + 1) * lineHeight);
//   }
// }

static void drawHoldPageFullBuffer() {
  const int lineHeight = 27;
  const int maxShown = 5;

  sortAircraftCacheByDistance();
  recalcPaging();

  // compose the entire frame once
  full_paint.Clear(UNCOLORED);

  // Header (no striping on header)
  char header[64];
  snprintf(header, sizeof(header), "HOLD | page %d/%d | Aircraft Total:%d",
           sCurrentPage, sTotalPages, sTotalActive);
  full_paint.DrawStringAt(0, 5, header, &Font16, COLORED);
  full_paint.DrawStringAt(1, 5, header, &Font16, COLORED); // offset by 1px for thickness

  int y = lineHeight;            // start drawing rows under header
  int startLogical = (sCurrentPage - 1) * maxShown;
  int logicalIdx   = 0;
  int shown        = 0;

  for (int i = 0; i < MAX_CACHE_SIZE && shown < maxShown; i++) {
    if (aircraftCache[i].icao24 == "" || aircraftCache[i].distance < 0) continue;
    if (logicalIdx++ < startLogical) continue;

    // Build text
    String dir = getCompassDirection(aircraftCache[i].bearing);
    String model = aircraftCache[i].model;
    String callsign = aircraftCache[i].callsign;
    String country = aircraftCache[i].country;
    if (callsign == "") callsign = aircraftCache[i].icao24;

    char paddedCallsign[8];
    snprintf(paddedCallsign, sizeof(paddedCallsign), "%-7s", callsign.c_str());
    char paddedDistance[7];
    snprintf(paddedDistance, sizeof(paddedDistance), "%6.2f", aircraftCache[i].distance);
    int bearingInt = (int)aircraftCache[i].bearing;
    char paddedBearing[4];
    snprintf(paddedBearing, sizeof(paddedBearing), "%3d", bearingInt);
    String compassDir = dir.substring(dir.length() - 2);

    char infoLine[64];
    snprintf(infoLine, sizeof(infoLine), "%s%skm %s%s %s",
             paddedCallsign, paddedDistance, paddedBearing, compassDir.c_str(), country.c_str());

    // --- zebra striping: two lines per aircraft share the same background ---
    bool dark = (shown % 2 == 0);  // dark block first, like your LIVE view

    // line 1 (model)
    if (dark) {
      full_paint.DrawFilledRectangle(0, y, 399, y + lineHeight - 1, COLORED);
      full_paint.DrawStringAt(5, y + 5, model.c_str(), &Font16, UNCOLORED);
    } else {
      // light
      // (background already white)
      full_paint.DrawStringAt(5, y + 5, model.c_str(), &Font16, COLORED);
    }
    y += lineHeight;

    // line 2 (info)
    if (dark) {
      full_paint.DrawFilledRectangle(0, y, 399, y + lineHeight - 1, COLORED);
      full_paint.DrawStringAt(5, y + 5, infoLine, &Font16, UNCOLORED);
    } else {
      full_paint.DrawStringAt(5, y + 5, infoLine, &Font16, COLORED);
    }
    y += lineHeight;

    shown++;
  }

  // push once (FULL refresh)
  epd.Init();
  // Do NOT call epd.Clear() if your driver’s full-frame display already clears internally.
  // If your driver expects a clear, keep it; otherwise remove the next line to avoid a double flash.
  // epd.Clear();

  // Use the correct full-frame call for your driver:
  // For Waveshare-style: epd.DisplayFrame(full_paint.GetImage());
  // For GxEPD2-like:     epd.Display(full_paint.GetImage());
  epd.Display(full_paint.GetImage());
  // If available: epd.WaitUntilIdle();
}


void drawAircraftInfoToDisplay_Partial(const char* timeStr, int /*totalAircraftFromAPI*/) {
  if (sMode == HOLD_MODE) {
    // HOLD always uses one full refresh and never draws partials
    drawHoldPageFullBuffer();
    return;
  }

  const int lineHeight = 27;
  const int maxShown   = 5;

  sortAircraftCacheByDistance();
  recalcPaging();

  // Cache last live timestamp for instant return to LIVE
  if (timeStr && timeStr[0]) {
    strncpy(sLastTimeStr, timeStr, sizeof(sLastTimeStr) - 1);
    sLastTimeStr[sizeof(sLastTimeStr) - 1] = '\0';
  }

  int currentLine = 0;
  int linesUsed   = 0;
  const int maxLines = 1 + (maxShown * 2);

  // Header — WRITE ONLY (no refresh)
  char header[64];
  snprintf(header, sizeof(header), "%s | Total:%d",
           (timeStr && timeStr[0]) ? timeStr : sLastTimeStr, sTotalActive);
  paint.Clear(UNCOLORED);
  paint.DrawStringAt(0, 5, header, &Font16, COLORED);
  epd.Display_Partial_Not_refresh(paint.GetImage(), 0, currentLine, 400, currentLine + lineHeight);
  currentLine += lineHeight; linesUsed++;

  // Rows — WRITE ONLY (no refresh)
  int logicalIdx = 0, shown = 0;
  for (int i = 0; i < MAX_CACHE_SIZE && shown < maxShown; i++) {
    if (aircraftCache[i].icao24 == "" || aircraftCache[i].distance < 0) continue;
    // LIVE shows first 5 only (startLogical = 0)
    (void)logicalIdx; // keep var to mirror HOLD logic if you expand later

    String dir = getCompassDirection(aircraftCache[i].bearing);
    String model = aircraftCache[i].model;
    String callsign = aircraftCache[i].callsign;
    String country = aircraftCache[i].country;
    if (callsign == "") callsign = aircraftCache[i].icao24;

    char paddedCallsign[8];
    snprintf(paddedCallsign, sizeof(paddedCallsign), "%-7s", callsign.c_str());
    char paddedDistance[7];
    snprintf(paddedDistance, sizeof(paddedDistance), "%6.2f", aircraftCache[i].distance);
    int bearingInt = (int)aircraftCache[i].bearing;
    char paddedBearing[4];
    snprintf(paddedBearing, sizeof(paddedBearing), "%3d", bearingInt);
    String compassDir = dir.substring(dir.length() - 2);

    char infoLine[64];
    snprintf(infoLine, sizeof(infoLine), "%s%skm %s%s %s",
             paddedCallsign, paddedDistance, paddedBearing, compassDir.c_str(), country.c_str());

    bool dark = (shown % 2 == 0);

    paint.Clear(dark ? COLORED : UNCOLORED);
    paint.DrawStringAt(5, 5, model.c_str(), &Font16, dark ? UNCOLORED : COLORED);
    epd.Display_Partial_Not_refresh(paint.GetImage(), 0, currentLine, 400, currentLine + lineHeight);
    currentLine += lineHeight; linesUsed++;

    paint.Clear(dark ? COLORED : UNCOLORED);
    paint.DrawStringAt(5, 5, infoLine, &Font16, dark ? UNCOLORED : COLORED);
    epd.Display_Partial_Not_refresh(paint.GetImage(), 0, currentLine, 400, currentLine + lineHeight);
    currentLine += lineHeight; linesUsed++;

    shown++;

    // Allow ISR to preempt before final refresh
    yield();
    if (gHoldRequested) return;
  }

  // Clear leftover rows — WRITE ONLY
  for (int i = linesUsed; i < maxLines; i++) {
    paint.Clear(UNCOLORED);
    epd.Display_Partial_Not_refresh(paint.GetImage(), 0, i * lineHeight, 400, (i + 1) * lineHeight);
    yield();
    if (gHoldRequested) return;
  }

  // Finalize exactly once
  if (!gHoldRequested) {
    epd.TurnOnDisplay_Partial();
  }
}


// ----- Public API used by main.cpp -----
void setDisplayMode(DisplayMode m) {
  if (sMode == m) return;
  sMode = m;
  if (sMode == HOLD_MODE) {
    sCurrentPage = 1;
    // DO NOT DRAW HERE  <- important to avoid double refresh
  } else {
    // Going back to LIVE -> draw once immediately with last time string
    epd.Init(); epd.Clear();
    drawAircraftInfoToDisplay_Partial(sLastTimeStr, 0);
  }
}

void pageUp()   { if (sMode==HOLD_MODE){ recalcPaging(); if (sCurrentPage < sTotalPages) sCurrentPage++; drawHoldPageFullBuffer(); } }
void pageDown() { if (sMode==HOLD_MODE){ recalcPaging(); if (sCurrentPage > 1) sCurrentPage--; drawHoldPageFullBuffer(); } }



void redrawHoldPageFull() { drawHoldPageFullBuffer(); }  // one FULL refresh