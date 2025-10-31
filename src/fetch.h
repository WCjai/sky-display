#pragma once
#include <Arduino.h>
#include "OpenSkyAuthClient.h"

struct BoundingBox { float south, north, west, east; };

BoundingBox getBoundingBox(float centerLat, float centerLon, int zoom);

/**
 * Fetches an aircraft model string for a given ICAO24.
 * The resolver consults the cache first, then walks a retry-aware pipeline:
 *   HexDB → OpenSky meta → PlaneSpotters → ADSB.one(desc/t) → ADSBdb (airline name)
 * Each provider is rate-limited, honours backoff, and negative results expire quickly.
 *
 * NOTE: Never caches "Unknown" (so later cycles can enrich).
 */
String fetchAircraftModel(const String& icao24,
                          OpenSkyAuthClient& auth,
                          const String& callsignOpt = "");

/**
 * Main polling function that:
 *  - pulls OpenSky states in a bounding box,
 *  - enriches callsign, route, and model,
 *  - updates the shared aircraftCache with distances/bearings,
 *  - draws to the display in LIVE mode.
 */
void fetchOpenSkyDataWithBoundingBox(float centerLat, float centerLon, int zoom,
                                     OpenSkyAuthClient& auth);
