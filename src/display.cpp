// display.cpp
#include "display.h"
#include "cache.h"

Epd epd;
unsigned char full_image[400 / 8 * 300];
Paint full_paint(full_image, 400, 300);

unsigned char image[400 / 8 * 28];
Paint paint(image, 400, 28);

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


void drawAircraftInfoToDisplay_Partial(const char* timeStr, int totalAircraft) {
  const int lineHeight = 27;
  const int maxShownAircraft = 5;
  const int maxLines = 1 + (maxShownAircraft * 2);
  int currentLine = 0;
  int linesUsed = 0;

  sortAircraftCacheByDistance();

  char header[64];
  snprintf(header, sizeof(header), "%s | Total:%d", timeStr, totalAircraft);
  paint.Clear(UNCOLORED);
  paint.DrawStringAt(0, 5, header, &Font16, COLORED);
  //paint.DrawStringAt(1, 5, header, &Font16, COLORED); 
  epd.Display_Partial(paint.GetImage(), 0, currentLine, 400, currentLine + lineHeight);
  currentLine += lineHeight;
  linesUsed++;

  int shown = 0;
  for (int i = 0; i < MAX_CACHE_SIZE && shown < maxShownAircraft; i++) {
    if (aircraftCache[i].icao24 != "" && aircraftCache[i].distance >= 0) {
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

      bool invertBlock = (shown % 2 == 0);

      paint.Clear(invertBlock ? COLORED : UNCOLORED);
      paint.DrawStringAt(5, 5, model.c_str(), &Font16, invertBlock ? UNCOLORED : COLORED);
      epd.Display_Partial(paint.GetImage(), 0, currentLine, 400, currentLine + lineHeight);
      currentLine += lineHeight;
      linesUsed++;

      paint.Clear(invertBlock ? COLORED : UNCOLORED);
      paint.DrawStringAt(5, 5, infoLine, &Font16, invertBlock ? UNCOLORED : COLORED);
      epd.Display_Partial(paint.GetImage(), 0, currentLine, 400, currentLine + lineHeight);
      currentLine += lineHeight;
      linesUsed++;

      shown++;
    }
  }

  for (int i = linesUsed; i < maxLines; i++) {
    paint.Clear(UNCOLORED);
    epd.Display_Partial(paint.GetImage(), 0, i * lineHeight, 400, (i + 1) * lineHeight);
  }
}
