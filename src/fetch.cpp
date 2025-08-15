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

// --- Add this helper near the top of fetch.cpp ---
static String normalizeManufacturer(const String& in) {
  String s = in; 
  s.trim();
  if (!s.length()) return s;

  String lower = s;
  lower.toLowerCase();

  // Map "Avions de Transport Regional" (and "Régional") to "ATR"
  if (lower.indexOf("avions de transport regional") != -1 ||
      lower.indexOf("avions de transport régional") != -1) {
    return "ATR";
  }

  // (Optional place to add more mappings later)
  // if (lower.indexOf("airbus industrie") != -1) return "Airbus";
  // if (lower.indexOf("the boeing company") != -1) return "Boeing";

  return s;
}


// Normalize manufacturers that are notoriously long


// Collapse multiple spaces to single spaces
static void collapseSpaces(String& s) {
  String out; out.reserve(s.length());
  bool ws = false;
  for (size_t i = 0; i < s.length(); ++i) {
    char c = s[i];
    if (c == ' ') {
      if (!ws) { out += ' '; ws = true; }
    } else {
      out += c; ws = false;
    }
  }
  s = out;
}

// Join up to three parts with single spaces (skip empties)
static String join3(const String& a, const String& b, const String& c) {
  String out;
  if (a.length()) out += a;
  if (b.length()) { if (out.length()) out += " "; out += b; }
  if (c.length()) { if (out.length()) out += " "; out += c; }
  if (out.length()) collapseSpaces(out);
  return out;
}

// Ellipsize to maxChars (try to cut at a space near the end)
static String ellipsize(const String& s, int maxChars) {
  if ((int)s.length() <= maxChars) return s;
  if (maxChars <= 1) return "…";
  int keep = maxChars - 1;
  String cut = s.substring(0, keep);
  int sp = cut.lastIndexOf(' ');
  if (sp >= keep - 8 && sp >= 8) cut = cut.substring(0, sp);
  cut += "…";
  return cut;
}


// ---- Replacement for fetchFromHexDB ----
static String fetchFromHexDB(const String& icao24) {
  HTTPClient http;
  http.setTimeout(15000);

  String hex = icao24; hex.toLowerCase();                // HexDB expects lowercase
  http.begin("https://hexdb.io/api/v1/aircraft/" + hex);
  int code = http.GET();
  if (code == 200) {
    String payload = http.getString();
    DynamicJsonDocument doc(3072);
    DeserializationError err = deserializeJson(doc, payload);
    if (!err) {
      if (doc.containsKey("error")) { http.end(); return ""; }

      // Pull fields
      String owners  = doc["RegisteredOwners"] | "";
      String opflag  = doc["OperatorFlagCode"] | "";
      String manuf   = doc["Manufacturer"]     | "";
      String type    = doc["Type"]             | "";
      String icao    = doc["ICAOTypeCode"]     | "";

      owners.trim(); opflag.trim(); manuf.trim(); type.trim(); icao.trim();

      // Sanitize manufacturer
      String manufN = normalizeManufacturer(manuf);

      // Width budget for model line (Font16 at x=5 on 400px panel)
      const int screenW = 400, leftPad = 5, rightPad = 5;
      const int maxChars = max(8, (screenW - leftPad - rightPad) / Font16.Width);
      auto fits = [&](const String& s){ return (int)s.length() <= maxChars; };

      // Priority: keep airline (owners) as long as possible, then try opflag
      // Sequence tries: 
      //   RO+Manuf+Type → RO+Manuf+ICAO → RO+ManufN+Type → RO+ManufN+ICAO
      //   OPF+Manuf+Type → OPF+Manuf+ICAO → OPF+ManufN+Type → OPF+ManufN+ICAO
      //   (still too long?) RO+ManufN → RO+ICAO → OPF+ManufN → OPF+ICAO
      //   If still long: RO+Type → OPF+Type → ManufN+ICAO → OPF → RO → ICAO → Type
      struct Cand { String a,b,c; };
      Cand seq[] = {
        { owners, manuf,  type },
        { owners, manuf,  icao },
        { owners, manufN, type },
        { owners, manufN, icao },

        { opflag, manuf,  type },
        { opflag, manuf,  icao },
        { opflag, manufN, type },
        { opflag, manufN, icao },

        { owners, manufN, ""   },
        { owners, "",     icao },
        { opflag, manufN, ""   },
        { opflag, "",     icao },

        { owners, "",     type },
        { opflag, "",     type },
        { manufN, "",     icao },
        { opflag, "",     ""   },
        { owners, "",     ""   },
        { "",      "",    icao },
        { "",      "",    type }
      };

      for (const auto& c : seq) {
        String s = join3(c.a, c.b, c.c);
        if (s.length() && fits(s)) { http.end(); return s; }
      }

      // Nothing fit: pick the most-informative short base that preserves airline,
      // then ellipsize to fit
      String base =
        join3(opflag, manufN, icao);             // OPF + manuf(short) + ICAO
      if (!base.length()) base = join3(opflag, "", icao);
      if (!base.length()) base = opflag;
      if (!base.length()) base = join3(owners, manufN, icao);
      if (!base.length()) base = icao;
      if (!base.length()) base = type;
      if (!base.length()) base = "Unknown";

      http.end();
      return ellipsize(base, maxChars);
    }
  }
  http.end();
  return "";
}




// --- ADSB.one: fill callsign from HEX when OpenSky has none ---
// === ADSB.one: fill callsign from HEX when OpenSky has none ===
static String fetchCallsignFromADSBOne(const String& icao24) {
  HTTPClient http;
  http.setTimeout(12000);

  String hex = icao24; hex.toUpperCase();
  http.begin("https://api.adsb.one/v2/hex/" + hex);
  int code = http.GET();
  if (code != 200) { http.end(); return ""; }

  DynamicJsonDocument doc(8192);
  DeserializationError err = deserializeJson(doc, http.getStream());
  http.end();
  if (err) return "";

  // Payload shape (your sample):
  // { "ac":[ { "flight":"UAE31T  ", "desc":"BOEING 777-300ER", "t":"B77W", ... } ], ... }
  JsonArray ac = doc["ac"].as<JsonArray>();
  if (ac.isNull() || ac.size() == 0) return "";

  String cs = (const char*)(ac[0]["flight"] | "");
  cs.trim();                  // remove trailing spaces in "UAE31T  "
  return cs;
}

// === ADSB.one: model fallback via 'desc' (else 't' ICAO type) ===
static String fetchModelFromADSBOneDesc(const String& icao24) {
  HTTPClient http;
  http.setTimeout(12000);

  String hex = icao24; hex.toUpperCase();
  http.begin("https://api.adsb.one/v2/hex/" + hex);
  int code = http.GET();
  if (code != 200) { http.end(); return ""; }

  DynamicJsonDocument doc(8192);
  DeserializationError err = deserializeJson(doc, http.getStream());
  http.end();
  if (err) return "";

  JsonArray ac = doc["ac"].as<JsonArray>();
  if (ac.isNull() || ac.size() == 0) return "";

  // Prefer 'desc' (full name), otherwise 't' (ICAO type code)
  String model = (const char*)(ac[0]["desc"] | "");
  if (!model.length()) model = (const char*)(ac[0]["t"] | "");
  model.trim();
  return model;
}



String fetchAircraftModel(const String& icao24, OpenSkyAuthClient& auth) {
  String cached = lookupCachedModel(icao24);
  if (cached.length()) return cached;

  // Try HexDB -> OpenSky -> ADSB.one(desc/t) -> PlaneSpotters
  String model = fetchFromHexDB(icao24);
  if (model == "") model = fetchFromOpenSkyMeta(icao24, auth);
  if (model == "") model =   fetchFromPlaneSpotters(icao24);// <-- NEW
  if (model == "") model = fetchModelFromADSBOneDesc(icao24);
  if (model == "") model = "Unknown";

  addToCache(icao24, model);
  return model;
}

// Turn "BLRTRZ" -> "BLR-TRZ" if it's exactly two IATA codes stuck together.
// Leaves everything else (e.g., "KLM879", "AAA→BBB") unchanged.
static String dashIfConcatenatedAirports(const String& in) {
  String s = in; s.trim();
  if (!s.length()) return s;

  // if it already contains a separator, leave it
  if (s.indexOf('-') >= 0 || s.indexOf('→') >= 0) return s;

  // make an uppercase copy for checks; return original casing in output
  String u = s; u.toUpperCase();

  // must be exactly 6 A–Z letters
  if (u.length() != 6) return s;
  for (int i = 0; i < 6; ++i) {
    char c = u[i];
    if (c < 'A' || c > 'Z') return s;
  }

  // looks like two IATA codes: insert dash and uppercase the codes
  return u.substring(0,3) + "-" + u.substring(3,6);
}

// Make "AAA→BBB" using IATA if present, else ICAO, else city/name.
// Returns "" if not enough info.
static String routeLabelFromFlightroute(JsonObject fr) {
  JsonObject o = fr["origin"];
  JsonObject d = fr["destination"];
  if (o.isNull() || d.isNull()) return "";

  String os = (String)(o["iata_code"] | "");
  String ds = (String)(d["iata_code"] | "");
  if (!os.length()) os = (String)(o["icao_code"] | "");
  if (!ds.length()) ds = (String)(d["icao_code"] | "");
  if (os.length() && ds.length()) return os + "-" + ds;


  return "";
}

// Query ADSBdb by callsign; return "SRC→DEST" or "" if unknown.
static String fetchRouteLabelForCallsign(String callsign) {
  callsign.trim();
  callsign.replace(" ", "");  // ADS-B callsigns sometimes include spaces
  if (!callsign.length()) return "";

  HTTPClient http;
  http.setTimeout(12000);
  http.begin("https://api.adsbdb.com/v0/callsign/" + callsign);
  int code = http.GET();
  if (code != 200) { http.end(); return ""; }

  // Response: { "response": { "flightroute": { origin:{...}, destination:{...}, ... } } }
  DynamicJsonDocument doc(4096);
  DeserializationError err = deserializeJson(doc, http.getStream());
  http.end();
  if (err) return "";

  JsonObject fr = doc["response"]["flightroute"];
  if (fr.isNull()) return "";
  return routeLabelFromFlightroute(fr);
}

// Decide what to show: route if available, else callsign, else ICAO24.
static String bestFlightLabel(const String& icao24, const String& callsign) {
  String route = fetchRouteLabelForCallsign(callsign);
  if (route.length()) return route;              // e.g., "BLR→TRZ" or "CityA-CityB"

  String cs = callsign; cs.trim();
  if (cs.length()) return dashIfConcatenatedAirports(cs);  // <- HERE

  return icao24;
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
  if (totalAircraft > MAX_AIRCRAFT_LIMIT) {
    // Draw persistent error screen
    drawTooManyAircraftScreen(totalAircraft);

    // Clean up HTTP and mark not busy
    http.end();
    isBusy = false;

    // HARD STOP: suspend this FreeRTOS task (self-suspend)
    // No more fetching until reboot or manual resume.
    vTaskSuspend(nullptr);
    return;
  }

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

      // Fill callsign first if OpenSky didn't provide it
      callsign.trim();
      if (!callsign.length()) {
        callsign = fetchCallsignFromADSBOne(icao24);  // <-- NEW
      }

      // (rest unchanged)
      float dist = haversineDistance(centerLat, centerLon, lat, lon);
      float brng = calculateBearing(centerLat, centerLon, lat, lon);

      String model       = fetchAircraftModel(icao24, auth);
      String flightLabel = bestFlightLabel(icao24, callsign);  // will query ADSBdb with filled callsign

      addToCache(icao24, model, flightLabel, country, dist, brng);

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
    ensurePartialPrimed();                 // stay in fast/partial
    if (totalAircraft == 0) {
      drawNoAircraftScreen(timestamp);     // partial write + TurnOnDisplay_Partial()
      lastHadAircraft = false;
    } else {
      drawAircraftInfoToDisplay_Partial(timeStr, totalAircraft);
      lastHadAircraft = true;
    }
  }

  // else (HOLD_MODE): DO NOT touch EPD here. We only updated the cache above.

  http.end();
  isBusy = false;
}

