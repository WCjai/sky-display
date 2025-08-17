// fetch.cpp — optimized, faithful to your old behavior

#include "fetch.h"
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>
#include <math.h>

#include "display.h"
#include "config.h"
#include "cache.h"

// externs from your project
extern DisplayMode getDisplayMode();
extern Epd epd;
extern SemaphoreHandle_t gCacheMutex;
//extern const FontDef Font16;  // used for width budgeting

// ---------------- Tunables ----------------
#define STATES_TIMEOUT_MS   20000
#define API_TIMEOUT_MS      15000
#define ADSB_TIMEOUT_MS     12000



// ---------------- Helpers ----------------

#include <map>

// ---------- lightweight caches ----------
struct RouteCacheEntry { String label; uint32_t expiryMs; };
static std::map<String, RouteCacheEntry> gRouteCache;   // key = callsign (normalized)

struct CallsignCacheEntry { String callsign; uint32_t expiryMs; };
static std::map<String, CallsignCacheEntry> gHexToCallsign; // key = icao24 (upper)

// route cache helpers
static void cacheRoute(const String& cs, const String& label, uint32_t ttlMsHit=10*60*1000UL, uint32_t ttlMsMiss=60*1000UL) {
  uint32_t now = millis();
  gRouteCache[cs] = { label, now + (label.length() ? ttlMsHit : ttlMsMiss) };
}
static String getCachedRoute(const String& cs) {
  auto it = gRouteCache.find(cs);
  if (it == gRouteCache.end()) return "";
  if ((int32_t)(it->second.expiryMs - millis()) <= 0) { gRouteCache.erase(it); return ""; }
  return it->second.label;
}

// callsign cache helpers
static void cacheCallsign(const String& hex, const String& cs, uint32_t ttlMsHit=2*60*1000UL, uint32_t ttlMsMiss=30*1000UL) {
  uint32_t now = millis();
  gHexToCallsign[hex] = { cs, now + (cs.length() ? ttlMsHit : ttlMsMiss) };
}
static String getCachedCallsign(const String& hex) {
  auto it = gHexToCallsign.find(hex);
  if (it == gHexToCallsign.end()) return "";
  if ((int32_t)(it->second.expiryMs - millis()) <= 0) { gHexToCallsign.erase(it); return ""; }
  return it->second.callsign;
}

// ---------- simple token buckets (keep you < ~8 rps) ----------
static uint32_t routeTokens = 8, routeLastRefill = 0;   // adsbdb
static uint32_t adsbTokens  = 8, adsbLastRefill  = 0;   // api.adsb.one

static bool allowRouteCall() {
  uint32_t now = millis();
  if (now - routeLastRefill >= 1000) {
    uint32_t add = (now - routeLastRefill) / 1000 * 8;
    routeTokens = min<uint32_t>(routeTokens + add, 16); // burst 16
    routeLastRefill = now;
  }
  if (!routeTokens) return false;
  routeTokens--;
  return true;
}
static bool allowADSBCall() {
  uint32_t now = millis();
  if (now - adsbLastRefill >= 1000) {
    uint32_t add = (now - adsbLastRefill) / 1000 * 8;
    adsbTokens = min<uint32_t>(adsbTokens + add, 16);
    adsbLastRefill = now;
  }
  if (!adsbTokens) return false;
  adsbTokens--;
  return true;
}

// ---------- small helper: use what we’ve already displayed for this ICAO ----------
static String lookupCachedDisplayLabelByIcao(const String& icao24) {
  if (!gCacheMutex || xSemaphoreTake(gCacheMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    for (int i = 0; i < MAX_CACHE_SIZE; ++i) {
      if (aircraftCache[i].icao24 == icao24 && aircraftCache[i].callsign.length()) {
        String v = aircraftCache[i].callsign;
        if (gCacheMutex) xSemaphoreGive(gCacheMutex);
        return v;
      }
    }
    if (gCacheMutex) xSemaphoreGive(gCacheMutex);
  }
  return "";
}


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
  const float R = 6371.0f;                         // km
  const float dLat = radians(lat2 - lat1);
  const float dLon = radians(lon2 - lon1);         // <-- ensure lon2 - lon1
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

static inline bool isMeaningfulModel(const String& sIn) {
  String s = sIn; s.trim();
  if (!s.length()) return false;
  if (s.equalsIgnoreCase("unknown")) return false;
  if (s.equalsIgnoreCase("null"))    return false;
  return true;
}

static String normalizeManufacturer(const String& in) {
  String s = in; 
  s.trim();
  if (!s.length()) return s;

  String lower = s; lower.toLowerCase();
  if (lower.indexOf("avions de transport regional") != -1 ||
      lower.indexOf("avions de transport régional") != -1) {
    return "ATR";
  }
  return s;
}

static void collapseSpaces(String& s) {
  String out; out.reserve(s.length());
  bool ws = false;
  for (size_t i = 0; i < s.length(); ++i) {
    char c = s[i];
    if (c == ' ') { if (!ws) { out += ' '; ws = true; } }
    else { out += c; ws = false; }
  }
  s = out;
}

static String join3(const String& a, const String& b, const String& c) {
  String out;
  if (a.length()) out += a;
  if (b.length()) { if (out.length()) out += " "; out += b; }
  if (c.length()) { if (out.length()) out += " "; out += c; }
  if (out.length()) collapseSpaces(out);
  return out;
}

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

// Turn "BLRTRZ" -> "BLR-TRZ" if it's exactly two IATA codes stuck together.
static String dashIfConcatenatedAirports(const String& in) {
  String s = in; s.trim();
  if (!s.length()) return s;
  if (s.indexOf('-') >= 0 || s.indexOf('→') >= 0) return s; // already formatted

  String u = s; u.toUpperCase();
  if (u.length() != 6) return s;
  for (int i = 0; i < 6; ++i) {
    char c = u[i];
    if (c < 'A' || c > 'Z') return s;
  }
  return u.substring(0,3) + "-" + u.substring(3,6);
}

// ---------------- External APIs ----------------
static String fetchFromPlaneSpotters(const String& icao24) {
  HTTPClient http;
  http.setTimeout(API_TIMEOUT_MS);
  http.begin("https://api.planespotters.net/pub/photos/hex/" + icao24);
  int code = http.GET();
  if (code == 200) {
    String payload = http.getString(); // small JSON — OK
    DynamicJsonDocument doc(4096);
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
  http.setTimeout(API_TIMEOUT_MS);
  http.begin("https://opensky-network.org/api/metadata/aircraft/icao/" + icao24);
  http.addHeader("Authorization", "Bearer " + auth.getAccessToken());
  int code = http.GET();
  if (code == 200) {
    String payload = http.getString();
    DynamicJsonDocument doc(2048);
    if (!deserializeJson(doc, payload)) {
      String model = doc["model"] | "";
      String oper  = doc["operator"] | "";
      model.trim(); oper.trim();
      http.end();
      String label = oper.length() ? (oper + " " + model) : model;
      return ellipsize(label, 36);
    }
  }
  http.end();
  return "";
}

static String fetchFromHexDB(const String& icao24) {
  HTTPClient http;
  http.setTimeout(15000);

  String hex = icao24; hex.toLowerCase();                // HexDB expects lowercase
  http.begin("https://hexdb.io/api/v1/aircraft/" + hex);
  int code = http.GET();
  if (code == 200) {
    String payload = http.getString();
    DynamicJsonDocument doc(4096);
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

      // ==== YOUR NEW PRIORITY ORDER ====
      struct Cand { String a,b,c; };
      Cand seq[] = {
        // RegisteredOwners first
        { owners, manuf,  type },
        { owners, manuf,  icao },
        { owners, manufN, type },
        { owners, manufN, icao },
        { owners, "",     icao },

        // Then OperatorFlagCode combos
        { opflag, manuf,  type },
        { opflag, manuf,  icao },
        { opflag, manufN, type },
        { opflag, manufN, icao },

        // Singles with normalized manufacturer
        { owners, manufN, ""   },
        { opflag, manufN, ""   },
        { opflag, "",     icao },

        // Type-only fallbacks by owner/opflag
        { owners, "",     type },
        { opflag, "",     type },

        // Manufacturer + ICAO
        { manufN, "",     icao },

        // Final minimal fallbacks
        { opflag, "",     ""   },
        { owners, "",     ""   },
        { "",      "",    icao },
        { "",      "",    type }
      };

      for (const auto& c : seq) {
        String s = join3(c.a, c.b, c.c);
        if (s.length() && fits(s)) { http.end(); return s; }
      }

      // Nothing fit: pick a compact base, then ellipsize to fit
      String base = join3(opflag, manufN, icao);   // short & informative
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


static String fetchCallsignFromADSBOne(const String& icao24) {
  String hex = icao24; hex.toUpperCase();

  // cache first
  if (String hit = getCachedCallsign(hex); hit.length() || hit == "") {
    if (hit.length() || /* negative cached */ gHexToCallsign.count(hex)) return hit;
  }

  if (!allowADSBCall()) return ""; // skip this tick if throttled

  HTTPClient http;
  http.setTimeout(ADSB_TIMEOUT_MS);
  http.begin("https://api.adsb.one/v2/hex/" + hex);
  int code = http.GET();
  if (code != 200) { http.end(); cacheCallsign(hex, ""); return ""; }

  DynamicJsonDocument doc(8192);
  DeserializationError err = deserializeJson(doc, http.getStream());
  http.end();
  if (err) { cacheCallsign(hex, ""); return ""; }

  JsonArray ac = doc["ac"].as<JsonArray>();
  if (ac.isNull() || ac.size() == 0) { cacheCallsign(hex, ""); return ""; }

  String cs = (const char*)(ac[0]["flight"] | "");
  cs.trim();
  cacheCallsign(hex, cs);
  return cs;
}


static String fetchModelFromADSBOneDesc(const String& icao24) {
  HTTPClient http;
  http.setTimeout(ADSB_TIMEOUT_MS);

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

  String model = (const char*)(ac[0]["desc"] | "");
  if (!model.length()) model = (const char*)(ac[0]["t"] | "");
  model.trim();
  return model;
}

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

static String fetchRouteLabelForCallsign(String callsign) {
  callsign.trim();
  callsign.replace(" ", "");
  if (!callsign.length()) return "";

  // route cache first
  if (String hit = getCachedRoute(callsign); hit.length() || /* negative cached */ gRouteCache.count(callsign))
    return hit;

  if (!allowRouteCall()) return ""; // skip for now

  HTTPClient http;
  http.setTimeout(API_TIMEOUT_MS);
  http.begin("https://api.adsbdb.com/v0/callsign/" + callsign);
  int code = http.GET();
  if (code != 200) { http.end(); cacheRoute(callsign, ""); return ""; }

  DynamicJsonDocument doc(4096);
  DeserializationError err = deserializeJson(doc, http.getStream());
  http.end();
  if (err) { cacheRoute(callsign, ""); return ""; }

  JsonObject fr = doc["response"]["flightroute"];
  String label = fr.isNull() ? "" : routeLabelFromFlightroute(fr);
  cacheRoute(callsign, label);
  return label;
}


static String bestFlightLabel(const String& icao24, const String& callsignIn) {
  // 0) if we already computed a display label for this ICAO, use it
  if (String v = lookupCachedDisplayLabelByIcao(icao24); v.length()) return v;

  // 1) normalize callsign
  String cs = callsignIn; cs.trim(); cs.replace(" ", "");
  if (!cs.length()) return icao24;   // no raw callsign → don’t try network

  // 2) route cache → network (once) → fallback to raw callsign (maybe "BLRTRZ" → "BLR-TRZ")
  if (String hit = getCachedRoute(cs); hit.length()) return hit;

  if (String route = fetchRouteLabelForCallsign(cs); route.length()) return route;

  return dashIfConcatenatedAirports(cs);
}

// ---------------- Public: aircraft model (old order, no caching "Unknown") ----------------
String fetchAircraftModel(const String& icao24, OpenSkyAuthClient& auth) {
  String cached = lookupCachedModel(icao24);
  if (isMeaningfulModel(cached)) return cached;

  // Try HexDB -> OpenSky -> PlaneSpotters -> ADSB.one(desc/t)
  String model = fetchFromHexDB(icao24);
  if (!isMeaningfulModel(model)) model = fetchFromOpenSkyMeta(icao24, auth);
  if (!isMeaningfulModel(model)) model = fetchFromPlaneSpotters(icao24);
  if (!isMeaningfulModel(model)) model = fetchModelFromADSBOneDesc(icao24);

  if (!isMeaningfulModel(model)) {
    // DON'T cache "Unknown" so later cycles can still enrich
    return "Unknown";
  }

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
  http.setTimeout(STATES_TIMEOUT_MS);
  http.useHTTP10(true);                        // robust path (avoids chunked issues)
  http.begin(url);
  http.addHeader("Authorization", "Bearer " + auth.getAccessToken());
  http.addHeader("Connection", "close");
  http.addHeader("Accept", "application/json");
  http.addHeader("Accept-Encoding", "identity");

  int httpCode = http.GET();
  if (httpCode != 200) {
    Serial.printf("[fetch] HTTP %d from states/all\n", httpCode);
    http.end();
    isBusy = false;
    return;
  }

  // Parse JSON (full parse, reliable)
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
    drawTooManyAircraftScreen(totalAircraft);
    http.end();
    isBusy = false;
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

        // guard against invalid coords
        if (fabs(lat) > 90.0f || fabs(lon) > 180.0f) continue;

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

  // ---- Phase 2: add/update rows (no long-held mutex) ----
  if (!states.isNull()) {
    for (JsonArray st : states) {
      String icao24   = st[0] | "";
      String callsign = st[1] | "";
      String country  = st[2] | "";
      float  lon      = st[5] | 0.0f;
      float  lat      = st[6] | 0.0f;
      if (icao24 == "" || lat == 0.0f || lon == 0.0f) continue;
      if (fabs(lat) > 90.0f || fabs(lon) > 180.0f) continue;

      // Fill callsign if OpenSky didn't provide it
      callsign.trim();
      if (!callsign.length()) {
        String hexU = icao24; hexU.toUpperCase();
        callsign = getCachedCallsign(hexU);
        if (!callsign.length()) callsign = fetchCallsignFromADSBOne(hexU);
      }


      float dist = haversineDistance(centerLat, centerLon, lat, lon);
      float brng = calculateBearing(centerLat, centerLon, lat, lon);

      // Get model (do NOT cache Unknown)
      String model = fetchAircraftModel(icao24, auth);

      // If model came back Unknown, keep any previous good value
      if (!isMeaningfulModel(model)) {
        String prev = lookupCachedModel(icao24);
        if (isMeaningfulModel(prev)) model = prev;
      }

      String flightLabel = bestFlightLabel(icao24, callsign);

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
    ensurePartialPrimed();
    if (totalAircraft == 0) {
      drawNoAircraftScreen((time_t)timestamp);
      lastHadAircraft = false;
    } else {
      drawAircraftInfoToDisplay_Partial(timeStr, totalAircraft);
      lastHadAircraft = true;
    }
  }

  http.end();
  isBusy = false;
}



// RegisteredOwners + Manufacturer + Type

// RegisteredOwners + Manufacturer + ICAOTypeCode

// RegisteredOwners + normalized(Manufacturer) + Type

// RegisteredOwners + normalized(Manufacturer) + ICAOTypeCode

// RegisteredOwners + ICAOTypeCode

// OperatorFlagCode + Manufacturer + Type

// OperatorFlagCode + Manufacturer + ICAOTypeCode

// OperatorFlagCode + normalized(Manufacturer) + Type

// OperatorFlagCode + normalized(Manufacturer) + ICAOTypeCode

// RegisteredOwners + normalized(Manufacturer)

// OperatorFlagCode + normalized(Manufacturer)

// OperatorFlagCode + ICAOTypeCode

// RegisteredOwners + Type

// OperatorFlagCode + Type

// normalized(Manufacturer) + ICAOTypeCode

// OperatorFlagCode

// RegisteredOwners

// ICAOTypeCode

// Type