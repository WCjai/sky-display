#include "fetch.h"
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "display.h"
#include "config.h"
#include "cache.h"

extern DisplayMode getDisplayMode();
extern Epd epd;

// ---------------- Helpers ----------------
BoundingBox getBoundingBox(float centerLat, float centerLon, int zoom) {
  float latSpan, lonSpan;
  switch (zoom) {
    case  6: latSpan = 8.0;  lonSpan = 10.0; break;
    case  7: latSpan = 5.0;  lonSpan = 7.0;  break;
    case  8: latSpan = 3.5;  lonSpan = 4.5;  break;
    case  9: latSpan = 2.5;  lonSpan = 3.0;  break;
    case 10: latSpan = 1.5;  lonSpan = 2.0;  break;
    case 11: latSpan = 0.9;  lonSpan = 1.5;  break;
    case 12: latSpan = 0.5;  lonSpan = 0.8;  break;
    case 13: latSpan = 0.3;  lonSpan = 0.5;  break;
    case 14: latSpan = 0.15; lonSpan = 0.25; break;
    case 15: latSpan = 0.08; lonSpan = 0.12; break;
    default: latSpan = 3.5;  lonSpan = 4.5;  break;
  }
  return { centerLat - latSpan, centerLat + latSpan, centerLon - lonSpan, centerLon + lonSpan };
}

float haversineDistance(float lat1, float lon1, float lat2, float lon2) {
  const float R = 6371.0f;
  const float dLat = radians(lat2 - lat1);
  const float dLon = radians(lon2 - lon1);
  const float a = sinf(dLat * 0.5f) * sinf(dLat * 0.5f) +
                  cosf(radians(lat1)) * cosf(radians(lat2)) *
                  sinf(dLon * 0.5f) * sinf(dLon * 0.5f);
  const float c = 2.0f * atanf(sqrtf(a) / sqrtf(1.0f - a));
  return R * c;
}

float calculateBearing(float lat1, float lon1, float lat2, float lon2) {
  const float dLon = radians(lon2 - lon1);
  const float y = sinf(dLon) * cosf(radians(lat2));
  const float x = cosf(radians(lat1)) * sinf(radians(lat2)) -
                  sinf(radians(lat1)) * cosf(radians(lat2)) * cosf(dLon);
  float brng = degrees(atan2f(y, x));
  if (brng < 0) brng += 360.0f;
  return brng;
}

static String fetchFromPlaneSpotters(const String& icao24) {
  HTTPClient http;
  http.setTimeout(15000);
  http.begin("https://api.planespotters.net/pub/photos/hex/" + icao24);
  int code = http.GET();
  if (code == 200) {
    String payload = http.getString(); // small JSON — OK
    DynamicJsonDocument doc(2048);
    if (!deserializeJson(doc, payload)) {
      JsonArray photos = doc["photos"].as<JsonArray>();
      if (!photos.isNull() && photos.size() > 0) {
        String link = photos[0]["link"] | "";
        if (link.length()) {
          int lastSlash = link.lastIndexOf('/');
          String slug = link.substring(lastSlash + 1);
          int q = slug.indexOf('?');
          if (q != -1) slug = slug.substring(0, q);
          slug.replace("-", " ");
          int f = slug.indexOf(' '), s = slug.indexOf(' ', f + 1);
          if (s != -1) {
            String model = slug.substring(s + 1);
            model.trim(); model.toUpperCase();
            http.end();
            return model;
          }
        }
      }
    }
  }
  http.end();
  return "";
}

static String fetchFromOpenSkyMeta(const String& icao24, OpenSkyAuthClient& auth) {
  HTTPClient http;
  http.setTimeout(15000);
  http.begin("https://opensky-network.org/api/metadata/aircraft/icao/" + icao24);
  http.addHeader("Authorization", "Bearer " + auth.getAccessToken());
  int code = http.GET();
  if (code == 200) {
    String payload = http.getString();
    DynamicJsonDocument doc(1024);
    if (!deserializeJson(doc, payload)) {
      String model = doc["model"] | "";
      String oper  = doc["operator"] | "";
      model.trim(); oper.trim();
      http.end();
      return oper.length() ? (oper + " " + model) : model;
    }
  }
  http.end();
  return "";
}

String fetchAircraftModel(const String& icao24, OpenSkyAuthClient& auth) {
  String cached = lookupCachedModel(icao24);
  if (cached.length()) return cached;

  String model = fetchFromPlaneSpotters(icao24);
  if (model == "") model = fetchFromOpenSkyMeta(icao24, auth);
  if (model == "") model = "Unknown";

  addToCache(icao24, model);
  return model;
}

// ---------------- Main fetch ----------------
void fetchOpenSkyDataWithBoundingBox(float centerLat, float centerLon, int zoom, OpenSkyAuthClient& auth) {
  static bool isBusy = false;
  static bool lastHadAircraft = true;   // track LIVE-only clears
  if (isBusy) return;
  isBusy = true;

  if (!auth.ensureValidToken()) {
    isBusy = false;
    if (getDisplayMode() == LIVE_MODE) drawStatusScreen("Token fetch error");
    return;
  }

  const BoundingBox box = getBoundingBox(centerLat, centerLon, zoom);
  String url = String("https://opensky-network.org/api/states/all?") +
               "lamin=" + String(box.south, 5) +
               "&lamax=" + String(box.north, 5) +
               "&lomin=" + String(box.west, 5)  +
               "&lomax=" + String(box.east, 5);

  HTTPClient http;
  http.setTimeout(20000);
  http.useHTTP10(true);
  http.begin(url);
  http.addHeader("Authorization", "Bearer " + auth.getAccessToken());
  http.addHeader("Connection", "close");

  int httpCode = http.GET();
  if (httpCode != 200) {
    Serial.printf("[fetch] HTTP %d from states/all\n", httpCode);
    http.end();
    isBusy = false;
    return;
  }

  // Parse JSON
  DynamicJsonDocument doc(64 * 1024);
  DeserializationError jerr = deserializeJson(doc, http.getStream());
  if (jerr) {
    Serial.printf("[fetch] JSON error: %s\n", jerr.c_str());
    http.end();
    isBusy = false;
    return;
  }

  long timestamp = doc["time"] | 0;
  JsonArray states = doc["states"].as<JsonArray>();
  int totalAircraft = states.isNull() ? 0 : states.size();
  Serial.printf("[fetch] parsed states: %d\n", totalAircraft);

  // Build time string for LIVE header
  char timeStr[64];
  time_t zoned = timestamp + TZ_minutes * 60;
  struct tm* ti = gmtime(&zoned);
  if (USE_24H) strftime(timeStr, sizeof(timeStr), "%Y-%m-%d | %H:%M:%S", ti);
  else         strftime(timeStr, sizeof(timeStr), "%Y-%m-%d | %I:%M:%S %p", ti);

  // ---- Phase 1: mark inactive + update known (mutex) ----
  bool locked1 = false;
  if (gCacheMutex) locked1 = (xSemaphoreTake(gCacheMutex, pdMS_TO_TICKS(800)) == pdTRUE);
  if (!gCacheMutex || locked1) {
    for (int i = 0; i < MAX_CACHE_SIZE; i++) aircraftCache[i].active = false;

    if (!states.isNull()) {
      for (JsonArray st : states) {
        String icao24   = st[0] | "";
        float  lon      = st[5] | 0.0f;
        float  lat      = st[6] | 0.0f;
        if (icao24 == "" || lat == 0.0f || lon == 0.0f) continue;

        float dist = haversineDistance(centerLat, centerLon, lat, lon);
        float brng = calculateBearing(centerLat, centerLon, lat, lon);

        for (int i = 0; i < MAX_CACHE_SIZE; i++) {
          if (aircraftCache[i].icao24 == icao24) {
            aircraftCache[i].distance = dist;
            aircraftCache[i].bearing  = brng;
            aircraftCache[i].active   = true;
            break;
          }
        }
      }
    }
  }
  if (locked1) xSemaphoreGive(gCacheMutex);

  // ---- Phase 2: add new rows (no long-held mutex) ----
  if (!states.isNull()) {
    for (JsonArray st : states) {
      String icao24   = st[0] | "";
      String callsign = st[1] | "";
      String country  = st[2] | "";
      float  lon      = st[5] | 0.0f;
      float  lat      = st[6] | 0.0f;
      if (icao24 == "" || lat == 0.0f || lon == 0.0f) continue;

      bool present = false;
      if (!gCacheMutex || xSemaphoreTake(gCacheMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        for (int i = 0; i < MAX_CACHE_SIZE; i++) {
          if (aircraftCache[i].icao24 == icao24 && aircraftCache[i].active) { present = true; break; }
        }
        if (gCacheMutex) xSemaphoreGive(gCacheMutex);
      }
      if (present) continue;

      float dist = haversineDistance(centerLat, centerLon, lat, lon);
      float brng = calculateBearing(centerLat, centerLon, lat, lon);

      String model = fetchAircraftModel(icao24, auth);
      addToCache(icao24, model, callsign, country, dist, brng);

      // mark new row active
      if (!gCacheMutex || xSemaphoreTake(gCacheMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        for (int i = 0; i < MAX_CACHE_SIZE; i++) {
          if (aircraftCache[i].icao24 == icao24) { 
            aircraftCache[i].active = true; 
            break; 
          }
        }
        if (gCacheMutex) xSemaphoreGive(gCacheMutex);
      }
    }
  }

  // ---- Phase 3: purge inactive ----
  bool locked3 = false;
  if (gCacheMutex) locked3 = (xSemaphoreTake(gCacheMutex, pdMS_TO_TICKS(200)) == pdTRUE);
  if (!gCacheMutex || locked3) {
    for (int i = 0; i < MAX_CACHE_SIZE; i++) {
      if (!aircraftCache[i].active) aircraftCache[i] = {};
    }
  }
  if (locked3) xSemaphoreGive(gCacheMutex);

  // ---- Draw only in LIVE ----
  if (getDisplayMode() == LIVE_MODE) {
    if (totalAircraft == 0) {
      if (lastHadAircraft) { epd.Init(); epd.Clear(); }
      drawNoAircraftScreen(timestamp);
      lastHadAircraft = false;
    } else {
      if (!lastHadAircraft) { epd.Init(); epd.Clear(); }
      drawAircraftInfoToDisplay_Partial(timeStr, totalAircraft);
      lastHadAircraft = true;
    }
  }
  // else (HOLD_MODE): DO NOT touch EPD here. We only updated the cache above.

  http.end();
  isBusy = false;
}

