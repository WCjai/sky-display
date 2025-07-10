// cache.h
#pragma once

#include <Arduino.h>

#define MAX_CACHE_SIZE 30

struct AircraftCacheEntry {
  String icao24;
  String model;
  String callsign;
  String country;
  float distance = -1;
  float bearing = -1;
  bool active = false;
};

extern AircraftCacheEntry aircraftCache[MAX_CACHE_SIZE];
extern int cacheIndex;

String getCompassDirection(float bearing);
void addToCache(const String& icao24, const String& model, const String& callsign = "", const String& country = "", float distance = -1, float bearing = -1);
String lookupCachedModel(const String& icao24);
void sortAircraftCacheByDistance();
void printAircraftCacheSorted();