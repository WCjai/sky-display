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



void drawStatusScreen(const char* msg);
void drawNoAircraftScreen(time_t timestamp);
void drawAircraftInfoToDisplay_Partial(const char* timeStr, int totalAircraft);
