#pragma once
#include <Arduino.h>
#include "epd4in2_V2.h"
#include "epdpaint.h"
#include "fonts.h"
#include <ctime>

// NEW: include FreeRTOS semaphore types
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#define COLORED 0
#define UNCOLORED 1

extern Epd epd;
extern Paint full_paint;
extern unsigned char full_image[];
extern bool USE_24H;
extern int TZ_minutes;

enum DisplayMode { LIVE_MODE, HOLD_MODE };

// NEW: declare the global display mutex (defined in display.cpp)
extern SemaphoreHandle_t gDisplayMutex;

// Call this once in setup() before any drawing:
void initDisplayMutex();

void setDisplayMode(DisplayMode m);
DisplayMode getDisplayMode();
void drawHoldPagePartial();
void drawTooManyAircraftScreen(int total);
void pageUp();
void pageDown();
void redrawHoldPageFull();
void drawStatusScreen(const char* msg);
void drawStatusScreenwithline(const char* line1,
                              const char* line2 = "",
                              const char* line3 = "",
                              const char* line4 = "",
                              const char* line5 = "",
                              const char* line6 = "");
void drawNoAircraftScreen(time_t timestamp);
int  getActiveCount();
void ensurePartialPrimed();
bool hasActiveAircraft();
void drawAircraftInfoToDisplay_Partial(const char* timeStr, int totalAircraft);
void debugShowDummyLiveBoot();
void debugShowDummyHoldBoot();