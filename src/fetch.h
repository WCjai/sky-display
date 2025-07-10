// fetch.h
#pragma once

#include <Arduino.h>
#include "OpenSkyAuthClient.h"
#include "cache.h"

struct BoundingBox {
  float south;
  float north;
  float west;
  float east;
};

BoundingBox getBoundingBox(float centerLat, float centerLon, int zoom);
void fetchOpenSkyDataWithBoundingBox(float centerLat, float centerLon, int zoom, OpenSkyAuthClient& auth);
String fetchAircraftModel(const String& icao24, OpenSkyAuthClient& auth);
float haversineDistance(float lat1, float lon1, float lat2, float lon2);
float calculateBearing(float lat1, float lon1, float lat2, float lon2);
