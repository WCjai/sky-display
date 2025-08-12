// display.h
#pragma once

#include <Arduino.h>
#include "epd4in2_V2.h"
#include "epdpaint.h"
#include "fonts.h"
#include <ctime>

#define COLORED 0
#define UNCOLORED 1

extern Epd epd;
extern Paint full_paint;
extern unsigned char full_image[];
extern bool USE_24H;
extern int TZ_minutes;



enum DisplayMode { LIVE_MODE, HOLD_MODE };

void setDisplayMode(DisplayMode m);   // will NOT draw when entering HOLD; WILL draw when entering LIVE
DisplayMode getDisplayMode();         // NEW: query current mode


void pageUp();                        // HOLD: next page (does one FULL refresh)
void pageDown();                      // HOLD: prev page (one FULL refresh)
void redrawHoldPageFull();            // HOLD: redraw current page once (FULL refresh)

void drawStatusScreen(const char* msg);
void drawStatusScreenwithline(const char* line1,
                              const char* line2 = "",
                              const char* line3 = "",
                              const char* line4 = "",
                              const char* line5 = "",
                              const char* line6 = "");
void drawNoAircraftScreen(time_t timestamp);
void drawAircraftInfoToDisplay_Partial(const char* timeStr, int totalAircraft);
