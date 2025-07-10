// cache.cpp
#include "cache.h"

AircraftCacheEntry aircraftCache[MAX_CACHE_SIZE];
int cacheIndex = 0;

String getCompassDirection(float bearing) {
  const char* directions[] = {
    "N ", "NE", "E ", "SE", "S ", "SW", "W ", "NW", "N "
  };
  int index = round(bearing / 45.0);
  return String((int)bearing) + directions[index];
}

void addToCache(const String& icao24, const String& model, const String& callsign, const String& country, float distance, float bearing) {
  for (int i = 0; i < MAX_CACHE_SIZE; i++) {
    if (aircraftCache[i].icao24 == icao24) {
      aircraftCache[i].model = model;
      aircraftCache[i].callsign = callsign;
      aircraftCache[i].country = country;
      aircraftCache[i].distance = distance;
      aircraftCache[i].bearing = bearing;
      return;
    }
  }

  aircraftCache[cacheIndex].icao24 = icao24;
  aircraftCache[cacheIndex].model = model;
  aircraftCache[cacheIndex].callsign = callsign;
  aircraftCache[cacheIndex].country = country;
  aircraftCache[cacheIndex].distance = distance;
  aircraftCache[cacheIndex].bearing = bearing;
  cacheIndex = (cacheIndex + 1) % MAX_CACHE_SIZE;
}

String lookupCachedModel(const String& icao24) {
  for (int i = 0; i < MAX_CACHE_SIZE; i++) {
    if (aircraftCache[i].icao24 == icao24) {
      return aircraftCache[i].model;
    }
  }
  return "";
}

void sortAircraftCacheByDistance() {
  for (int i = 0; i < MAX_CACHE_SIZE - 1; i++) {
    for (int j = i + 1; j < MAX_CACHE_SIZE; j++) {
      if (aircraftCache[j].distance >= 0 && 
          (aircraftCache[i].distance < 0 || aircraftCache[j].distance < aircraftCache[i].distance)) {
        AircraftCacheEntry temp = aircraftCache[i];
        aircraftCache[i] = aircraftCache[j];
        aircraftCache[j] = temp;
      }
    }
  }
}

void printAircraftCacheSorted() {
  sortAircraftCacheByDistance();
  for (int i = 0; i < MAX_CACHE_SIZE; i++) {
    if (aircraftCache[i].icao24 != "" && aircraftCache[i].distance >= 0) {
      String direction = getCompassDirection(aircraftCache[i].bearing);
      Serial.printf("✈️ %s — %.2f km — %s — [%s]\n",
          aircraftCache[i].model.c_str(),
          aircraftCache[i].distance,
          direction.c_str(),
          aircraftCache[i].callsign != "" ? aircraftCache[i].callsign.c_str() : aircraftCache[i].icao24.c_str()
      );
    }
  }
}
