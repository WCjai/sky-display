#pragma once

#include <Arduino.h>
#include "OpenSkyAuthClient.h"
#include "cache.h"
#include <freertos/task.h> 

#define MAX_AIRCRAFT_LIMIT 100


// View window based on zoom & home position
struct BoundingBox {
  float south;
  float north;
  float west;
  float east;
};

BoundingBox getBoundingBox(float centerLat, float centerLon, int zoom);

// Core fetch (can be called from a FreeRTOS task)
void fetchOpenSkyDataWithBoundingBox(float centerLat, float centerLon, int zoom, OpenSkyAuthClient& auth);

// Utilities used across the project
String fetchAircraftModel(const String& icao24, OpenSkyAuthClient& auth);
float haversineDistance(float lat1, float lon1, float lat2, float lon2);
float calculateBearing(float lat1, float lon1, float lat2, float lon2);