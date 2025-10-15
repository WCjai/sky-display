#pragma once
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// Tune to your UI / memory
#ifndef MAX_CACHE_SIZE
#define MAX_CACHE_SIZE 64
#endif

struct AircraftCacheEntry {
  String icao24;
  String model;
  String callsign;
  String country;
  float  distance = -1.0f;
  float  bearing  = -1.0f;
  bool   active   = false;

  // Freshness metrics (used for smarter eviction / pruning)
  uint32_t lastSeenMs = 0;
  uint16_t seenCount  = 0;
};

extern AircraftCacheEntry aircraftCache[MAX_CACHE_SIZE];
extern int cacheIndex;

// Global mutex protecting the cache rows
extern SemaphoreHandle_t gCacheMutex;
void initCacheMutex();

// Lookup helpers
String lookupCachedModel(const String& icao24);

// Add/update (never-degrade writes; smart victim)
void addToCache(const String& icao24,
                const String& model,
                const String& callsign,
                const String& country,
                float distance,
                float bearing);

// Optional utilities
void touchIcao(const String& icao24);          // mark as seen (updates lastSeenMs/seenCount)
void pruneCacheByAge(uint32_t maxAgeMs);       // free rows not seen since maxAgeMs
void printAircraftCacheSorted();

// Pretty bearing for Serial logs (optional)
String getCompassDirection(float bearing);
