#pragma once
#include <Arduino.h>
#include "OpenSkyAuthClient.h"

struct BoundingBox {
  float south;
  float north;
  float west;
  float east;
};

// Main entry: pulls OpenSky states in a bbox, enriches, updates cache, draws
void fetchOpenSkyDataWithBoundingBox(float centerLat, float centerLon, int zoom, OpenSkyAuthClient& auth);

// Returns best-known aircraft model for an ICAO (never degrades prior good)
String fetchAircraftModel(const String& icao24, OpenSkyAuthClient& auth);

// Utility (exported if you need it elsewhere)
BoundingBox getBoundingBox(float centerLat, float centerLon, int zoom);
