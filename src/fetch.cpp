// fetch.cpp
#include "fetch.h"
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "display.h"
#include "config.h"  // for TZ_minutes and USE_24H

BoundingBox getBoundingBox(float centerLat, float centerLon, int zoom) {
  float latSpan, lonSpan;
  switch (zoom) {
    case 6:  latSpan = 8.0;  lonSpan = 10.0; break;
    case 7:  latSpan = 5.0;  lonSpan = 7.0;  break;
    case 8:  latSpan = 3.5;  lonSpan = 4.5;  break;
    case 9:  latSpan = 2.5;  lonSpan = 3.0;  break;
    case 10: latSpan = 1.5;  lonSpan = 2.0;  break;
    case 11: latSpan = 0.9;  lonSpan = 1.5;  break;
    case 12: latSpan = 0.5;  lonSpan = 0.8;  break;
    case 13: latSpan = 0.3;  lonSpan = 0.5;  break;
    case 14: latSpan = 0.15; lonSpan = 0.25; break;
    case 15: latSpan = 0.08; lonSpan = 0.12; break;
    default: latSpan = 3.5;  lonSpan = 4.5;  break;
  }
  return {
    centerLat - latSpan,
    centerLat + latSpan,
    centerLon - lonSpan,
    centerLon + lonSpan
  };
}

float haversineDistance(float lat1, float lon1, float lat2, float lon2) {
  const float R = 6371.0;
  float dLat = radians(lat2 - lat1);
  float dLon = radians(lon2 - lon1);

  float a = sin(dLat / 2) * sin(dLat / 2) +
            cos(radians(lat1)) * cos(radians(lat2)) *
            sin(dLon / 2) * sin(dLon / 2);
  float c = 2 * atan2(sqrt(a), sqrt(1 - a));

  return R * c;
}

float calculateBearing(float lat1, float lon1, float lat2, float lon2) {
  float dLon = radians(lon2 - lon1);
  float y = sin(dLon) * cos(radians(lat2));
  float x = cos(radians(lat1)) * sin(radians(lat2)) -
            sin(radians(lat1)) * cos(radians(lat2)) * cos(dLon);
  float brng = atan2(y, x);
  brng = degrees(brng);
  return fmod((brng + 360.0), 360.0);
}

String fetchFromPlaneSpotters(const String& icao24) {
  String url = "https://api.planespotters.net/pub/photos/hex/" + icao24;
  HTTPClient http;
  http.begin(url);
  int httpCode = http.GET();

  if (httpCode == 200) {
    String payload = http.getString();
    DynamicJsonDocument doc(2048);
    if (!deserializeJson(doc, payload)) {
      JsonArray photos = doc["photos"].as<JsonArray>();
      if (!photos.isNull() && photos.size() > 0) {
        String link = photos[0]["link"] | "";
        if (link != "") {
          int lastSlash = link.lastIndexOf('/');
          String slug = link.substring(lastSlash + 1);
          int qMark = slug.indexOf('?');
          if (qMark != -1) slug = slug.substring(0, qMark);
          slug.replace("-", " ");
          int firstSpace = slug.indexOf(' ');
          int secondSpace = slug.indexOf(' ', firstSpace + 1);
          if (secondSpace != -1) {
            String model = slug.substring(secondSpace + 1);
            model.trim();
            model.toUpperCase();
            return model;
          }
        }
      }
    }
  }
  return "";
}

String fetchFromOpenSky(const String& icao24, OpenSkyAuthClient& auth) {
  String url = "https://opensky-network.org/api/metadata/aircraft/icao/" + icao24;
  HTTPClient http;
  http.begin(url);
  http.addHeader("Authorization", "Bearer " + auth.getAccessToken());

  int httpCode = http.GET();
  if (httpCode == 200) {
    String payload = http.getString();
    DynamicJsonDocument doc(1024);
    if (!deserializeJson(doc, payload)) {
      String model = doc["model"] | "";
      String operators = doc["operator"] | "";
      model.trim(); operators.trim();
      return operators != "" ? operators + model : model;
    }
  }
  return "";
}

String fetchAircraftModel(const String& icao24, OpenSkyAuthClient& auth) {
  for (int i = 0; i < MAX_CACHE_SIZE; i++) {
    if (aircraftCache[i].icao24 == icao24) return aircraftCache[i].model;
  }
  String model = fetchFromPlaneSpotters(icao24);
  if (model == "") model = fetchFromOpenSky(icao24, auth);
  if (model == "") model = "Unknown";
  addToCache(icao24, model);
  return model;
}

void fetchOpenSkyDataWithBoundingBox(float centerLat, float centerLon, int zoom, OpenSkyAuthClient& auth) {
  static bool isBusy = false;
  static bool lastHadAircraft = true;
  if (isBusy) return;
  isBusy = true;

  if (!auth.ensureValidToken()) {
    isBusy = false;
    drawStatusScreen("Token fetch error");
    return;
  }

  for (int i = 0; i < MAX_CACHE_SIZE; i++) aircraftCache[i].active = false;

  BoundingBox box = getBoundingBox(centerLat, centerLon, zoom);
  String url = String("https://opensky-network.org/api/states/all?") +
               "lamin=" + String(box.south, 5) +
               "&lamax=" + String(box.north, 5) +
               "&lomin=" + String(box.west, 5) +
               "&lomax=" + String(box.east, 5);

  HTTPClient http;
  http.begin(url);
  http.addHeader("Authorization", "Bearer " + auth.getAccessToken());

  int httpCode = http.GET();
  if (httpCode == 200) {
    String payload = http.getString();
    DynamicJsonDocument doc(40 * 1024);
    if (!deserializeJson(doc, payload)) {
      long timestamp = doc["time"] | 0;
      JsonArray states = doc["states"].as<JsonArray>();
      int totalAircraft = states.size();

      char timeStr[64];
      time_t zoned = timestamp + TZ_minutes * 60;
      struct tm* ti = gmtime(&zoned);
      if (USE_24H) {
        strftime(timeStr, sizeof(timeStr), "%Y-%m-%d | %H:%M:%S", ti);
      } else {
        strftime(timeStr, sizeof(timeStr), "%Y-%m-%d | %I:%M:%S %p", ti);
      }

        

        if (totalAircraft == 0) {
            if (lastHadAircraft) {
                epd.Init();
                epd.Clear();
            }
            drawNoAircraftScreen(timestamp);
            drawNoAircraftScreen(timestamp);
            lastHadAircraft = false;
        } else {
            if (!lastHadAircraft) {
                epd.Init();
                epd.Clear();
            }
            drawAircraftInfoToDisplay_Partial(timeStr, totalAircraft);
            drawAircraftInfoToDisplay_Partial(timeStr, totalAircraft);
            lastHadAircraft = true;
        }


      for (JsonArray state : states) {
        String icao24 = state[0] | "";
        String callsign = state[1] | "";
        String country = state[2] | "";
        float lon = state[5] | 0.0;
        float lat = state[6] | 0.0;

        if (icao24 != "" && lat != 0.0 && lon != 0.0) {
          float dist = haversineDistance(centerLat, centerLon, lat, lon);
          float brng = calculateBearing(centerLat, centerLon, lat, lon);

          bool found = false;
          for (int i = 0; i < MAX_CACHE_SIZE; i++) {
            if (aircraftCache[i].icao24 == icao24) {
              aircraftCache[i].distance = dist;
              aircraftCache[i].bearing = brng;
              aircraftCache[i].active = true;
              found = true;
              break;
            }
          }

          if (!found) {
            String model = fetchAircraftModel(icao24, auth);
            addToCache(icao24, model, callsign, country, dist, brng);
            for (int i = 0; i < MAX_CACHE_SIZE; i++) {
              if (aircraftCache[i].icao24 == icao24) {
                aircraftCache[i].active = true;
                break;
              }
            }
          }
        }
      }

      for (int i = 0; i < MAX_CACHE_SIZE; i++) {
        if (!aircraftCache[i].active) aircraftCache[i] = {};
      }
    }
  }
  http.end();
  isBusy = false;
}
