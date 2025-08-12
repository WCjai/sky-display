#pragma once
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#define MAX_CACHE_SIZE 30

struct AircraftCacheEntry {
  String icao24;   // hex id
  String model;    // aircraft type/operator
  String callsign; // may be empty
  String country;  // origin country
  float  distance = -1.0f;
  float  bearing  = -1.0f;
  bool   active   = false;
};

extern AircraftCacheEntry aircraftCache[MAX_CACHE_SIZE];
extern int cacheIndex;

// Global cache mutex (created once at boot)
extern SemaphoreHandle_t gCacheMutex;
void initCacheMutex();

// Helpers
String getCompassDirection(float bearing);

// Thread-safe cache ops (all lock the mutex internally)
void addToCache(const String& icao24,
                const String& model,
                const String& callsign = "",
                const String& country  = "",
                float distance = -1.0f,
                float bearing  = -1.0f);

// Returns model if already present, else ""
String lookupCachedModel(const String& icao24);

// Only for debug printing to Serial (copies to a local list then sorts)
void printAircraftCacheSorted();
