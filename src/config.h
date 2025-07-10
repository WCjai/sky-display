// config.h
#pragma once

#include <Arduino.h>


extern String WIFI_SSID, WIFI_PASSWORD;
extern String CLIENT_ID, CLIENT_SECRET;
extern float HOME_LAT, HOME_LON;
extern uint8_t ZOOM;
extern String TZ;
extern int TZ_minutes;
extern bool USE_24H;

void loadConfig();
void startConfigMode();
