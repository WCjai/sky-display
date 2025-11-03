#include "fetch.h"

#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <map>
#include <math.h>

#include "display.h"
#include "config.h"
#include "cache.h"

// ===== DEBUG SWITCHES (set to 1 to enable) =====
#define DEBUG_FETCH   1   // high-level flow (OpenSky pull, per-aircraft summary)
#define DEBUG_MODEL   1   // model provider hits/misses (HexDB/OpenSky/PlaneSpotters/ADSB.one/ADSBdb-airline)
#define DEBUG_ROUTE   1   // callsign + route (ADSB.one callsign & ADSBdb route)
#define DEBUG_CACHE   1   // cache pruning sizes
#define DEBUG_HTTP    1   // per-request URL + HTTP code
#define DEBUG_HEAP    0   // heap watermark prints (ESP-IDF/Arduino)

// ===== helpers =====
#if DEBUG_HEAP
  #include "esp_heap_caps.h"
  static void dbg_heap(const char* tag){
    Serial.printf("[HEAP] %s | free=%u  min=%u\n", tag,
      heap_caps_get_free_size(MALLOC_CAP_DEFAULT),
      heap_caps_get_minimum_free_size(MALLOC_CAP_DEFAULT));
  }
#else
  static inline void dbg_heap(const char*) {}
#endif

#if DEBUG_FETCH || DEBUG_MODEL || DEBUG_ROUTE || DEBUG_CACHE || DEBUG_HTTP
  #define DBG_PRINTF(...)   do{ Serial.printf(__VA_ARGS__); }while(0)
#else
  #define DBG_PRINTF(...)   do{}while(0)
#endif

#if DEBUG_HTTP
  #define DBG_HTTP(...)     DBG_PRINTF(__VA_ARGS__)
#else
  #define DBG_HTTP(...)     do{}while(0)
#endif

#if DEBUG_MODEL
  #define DBG_MODEL(...)    DBG_PRINTF(__VA_ARGS__)
#else
  #define DBG_MODEL(...)    do{}while(0)
#endif

#if DEBUG_ROUTE
  #define DBG_ROUTE(...)    DBG_PRINTF(__VA_ARGS__)
#else
  #define DBG_ROUTE(...)    do{}while(0)
#endif

#if DEBUG_FETCH
  #define DBG_FETCH(...)    DBG_PRINTF(__VA_ARGS__)
#else
  #define DBG_FETCH(...)    do{}while(0)
#endif

#if DEBUG_CACHE
  #define DBG_CACHE(...)    DBG_PRINTF(__VA_ARGS__)
#else
  #define DBG_CACHE(...)    do{}while(0)
#endif

// ---------------- Tunables ----------------
#define STATES_TIMEOUT_MS   20000
#define API_TIMEOUT_MS      15000
#define ADSB_TIMEOUT_MS     12000

// Route/callsign cache hard caps
static const size_t ROUTE_MAX_ENTRIES    = 512;
static const size_t CALLSIGN_MAX_ENTRIES = 512;
static const size_t AIRLINE_MAX_ENTRIES  = 512;
static constexpr int MAX_MODEL_CHARS = 36;
// Pruning cadence
static uint32_t gLastPruneMs = 0;

// Backoff for model fetchers when providers are failing
static uint32_t gModelFailUntilMs = 0;

// ---------------- lightweight caches ----------------
struct RouteCacheEntry { String label; uint32_t expiryMs; };
static std::map<String, RouteCacheEntry> gRouteCache;   // key = callsign (normalized)

struct CallsignCacheEntry { String callsign; uint32_t expiryMs; };
static std::map<String, CallsignCacheEntry> gHexToCallsign; // key = icao24 (UPPER)

// NEW: callsign → airline.name cache (for airline-name-as-model fallback)
struct AirlineCacheEntry { String name; uint32_t expiryMs; };
static std::map<String, AirlineCacheEntry> gCallsignToAirline; // key = callsign (normalized)

// ---------- token buckets (keep you < ~8 rps each provider) ----------
static uint32_t routeTokens = 8, routeLastRefill = 0;   // adsbdb
static uint32_t adsbTokens  = 8, adsbLastRefill  = 0;   // api.adsb.one

static bool allowRouteCall() {
  uint32_t now = millis();
  if (now - routeLastRefill >= 1000) {
    uint32_t add = ((now - routeLastRefill) / 1000) * 8;
    routeTokens = min<uint32_t>(routeTokens + add, 16); // allow small bursts
    routeLastRefill = now;
  }
  if (!routeTokens) return false;
  routeTokens--;
  return true;
}
static bool allowADSBCall() {
  uint32_t now = millis();
  if (now - adsbLastRefill >= 1000) {
    uint32_t add = ((now - adsbLastRefill) / 1000) * 8;
    adsbTokens = min<uint32_t>(adsbTokens + add, 16);
    adsbLastRefill = now;
  }
  if (!adsbTokens) return false;
  adsbTokens--;
  return true;
}

// ---------- helpers ----------
static inline bool isMeaningfulModel(const String& sIn) {
  String s = sIn; s.trim();
  if (!s.length()) return false;
  if (s.equalsIgnoreCase("unknown")) return false;
  if (s.equalsIgnoreCase("null"))    return false;
  return true;
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
  out.reserve(a.length() + b.length() + c.length() + 2);
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

// Uppercase & trim ICAO type (e.g., "B738")
static String normalizeIcaoType(const String& in) {
  String s = in; s.trim(); s.toUpperCase();
  return s;
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

// Build label using your requested priority list, clipped to width
// Priority order (first that fits within maxChars):
// 1) airline + manufacturer + type
// 2) airline + manufacturer + icao_type
// 3) airline + icao_type
// 4) manufacturer + type
// 5) manufacturer + icao_type
// 6) airline
static String composeModelLabelPriority(String airline,
                                        String manufacturer,
                                        String type,
                                        String icao_type,
                                        int maxChars = MAX_MODEL_CHARS)
{
  airline.trim(); manufacturer.trim(); type.trim(); icao_type = normalizeIcaoType(icao_type);
  manufacturer = normalizeManufacturer(manufacturer);

  struct Cand { String a,b,c; };
  Cand seq[] = {
    { airline,      manufacturer, type      },  // 1
    { airline,      manufacturer, icao_type },  // 2
    { airline,      "",           icao_type },  // 3
    { manufacturer, "",           type      },  // 4
    { manufacturer, "",           icao_type },  // 5
    { airline,      "",           ""        },  // 6
  };

  for (const auto& c : seq) {
    String s = join3(c.a, c.b, c.c);
    s.trim();
    if (!s.length()) continue;
    if ((int)s.length() <= maxChars) return s;
  }

  // Nothing fit exactly → ellipsize the highest-value candidate that has content.
  // Try in priority order again, but ellipsize.
  for (const auto& c : seq) {
    String s = join3(c.a, c.b, c.c);
    s.trim();
    if (!s.length()) continue;
    return ellipsize(s, maxChars);
  }
  return "";
}



// Lookup the last label we rendered for this ICAO (from aircraftCache)
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
static bool routeCacheHas(const String& cs) {
  return gRouteCache.find(cs) != gRouteCache.end();
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

// airline cache helpers (callsign → airline name)
static void cacheAirline(const String& cs, const String& name, uint32_t ttlMsHit=10*60*1000UL, uint32_t ttlMsMiss=60*1000UL) {
  uint32_t now = millis();
  gCallsignToAirline[cs] = { name, now + (name.length() ? ttlMsHit : ttlMsMiss) };
}
static String getCachedAirline(const String& cs) {
  auto it = gCallsignToAirline.find(cs);
  if (it == gCallsignToAirline.end()) return "";
  if ((int32_t)(it->second.expiryMs - millis()) <= 0) { gCallsignToAirline.erase(it); return ""; }
  return it->second.name;
}

// map pruning
static void pruneSmallMaps() {
  const uint32_t now = millis();
  if ((uint32_t)(now - gLastPruneMs) < 30000UL) return; // every 30s
  gLastPruneMs = now;

  size_t beforeR = gRouteCache.size(), beforeC = gHexToCallsign.size(), beforeA = gCallsignToAirline.size();

  // remove expired entries
  for (auto it = gRouteCache.begin(); it != gRouteCache.end(); ) {
    if ((int32_t)(it->second.expiryMs - now) <= 0) it = gRouteCache.erase(it);
    else ++it;
  }
  for (auto it = gHexToCallsign.begin(); it != gHexToCallsign.end(); ) {
    if ((int32_t)(it->second.expiryMs - now) <= 0) it = gHexToCallsign.erase(it);
    else ++it;
  }
  for (auto it = gCallsignToAirline.begin(); it != gCallsignToAirline.end(); ) {
    if ((int32_t)(it->second.expiryMs - now) <= 0) it = gCallsignToAirline.erase(it);
    else ++it;
  }

  // hard caps (simple erase from begin; maps are small)
  while (gRouteCache.size()    > ROUTE_MAX_ENTRIES    && !gRouteCache.empty())    gRouteCache.erase(gRouteCache.begin());
  while (gHexToCallsign.size() > CALLSIGN_MAX_ENTRIES && !gHexToCallsign.empty()) gHexToCallsign.erase(gHexToCallsign.begin());
  while (gCallsignToAirline.size() > AIRLINE_MAX_ENTRIES && !gCallsignToAirline.empty()) gCallsignToAirline.erase(gCallsignToAirline.begin());

  DBG_CACHE("[RCACHE] size=%u  [CCACHE] size=%u  [ACACHE] size=%u (before %u/%u/%u)\n",
    (unsigned)gRouteCache.size(), (unsigned)gHexToCallsign.size(), (unsigned)gCallsignToAirline.size(),
    (unsigned)beforeR, (unsigned)beforeC, (unsigned)beforeA);
}

// ---------------- Geometry helpers ----------------
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

static float haversineDistance(float lat1, float lon1, float lat2, float lon2) {
  const float R = 6371.0f;
  const float dLat = radians(lat2 - lat1);
  const float dLon = radians(lon2 - lon1);
  const float a = sinf(dLat * 0.5f) * sinf(dLat * 0.5f) +
                  cosf(radians(lat1)) * cosf(radians(lat2)) *
                  sinf(dLon * 0.5f) * sinf(dLon * 0.5f);
  const float c = 2.0f * atanf(sqrtf(a) / sqrtf(1.0f - a));
  return R * c;
}
static float calculateBearing(float lat1, float lon1, float lat2, float lon2) {
  const float dLon = radians(lon2 - lon1);
  const float y = sinf(dLon) * cosf(radians(lat2));
  const float x = cosf(radians(lat1)) * sinf(radians(lat2)) -
                  sinf(radians(lat1)) * cosf(radians(lat2)) * cosf(dLon);
  float brng = degrees(atan2f(y, x));
  if (brng < 0) brng += 360.0f;
  return brng;
}


// ---------------- External APIs ----------------
static String fetchFromPlaneSpotters(const String& icao24) {
  HTTPClient http;
  http.setTimeout(API_TIMEOUT_MS);
  String url = "https://api.planespotters.net/pub/photos/hex/" + icao24;
  DBG_HTTP("[HTTP] GET %s\n", url.c_str());
  http.begin(url);
  int code = http.GET();
  DBG_HTTP("[HTTP] <- %d (PlaneSpotters)\n", code);
  if (code == 200) {
    String payload = http.getString();
    JsonDocument doc; // safe headroom
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
            DBG_MODEL("[MODEL] PlaneSpotters '%s'\n", model.c_str());
            http.end();
            return model;
          }
        }
      }
    } else {
      DBG_MODEL("[MODEL] PlaneSpotters JSON error\n");
    }
  }
  http.end();
  return "";
}

static String fetchFromOpenSkyMeta(const String& icao24, OpenSkyAuthClient& auth) {
  HTTPClient http;
  http.setTimeout(API_TIMEOUT_MS);
  String url = "https://opensky-network.org/api/metadata/aircraft/icao/" + icao24;
  DBG_HTTP("[HTTP] GET %s\n", url.c_str());
  http.begin(url);
  http.addHeader("Authorization", "Bearer " + auth.getAccessToken());
  int code = http.GET();
  DBG_HTTP("[HTTP] <- %d (OpenSky meta)\n", code);
  if (code == 200) {
    String payload = http.getString();
    JsonDocument doc;
    if (!deserializeJson(doc, payload)) {
      String model = doc["model"] | "";
      String oper  = doc["operator"] | "";
      model.trim(); oper.trim();
      String label = oper.length() ? (oper + " " + model) : model;
      label = ellipsize(label, 36);
      DBG_MODEL("[MODEL] OpenSky '%s'\n", label.c_str());
      http.end();
      return label;
    } else {
      DBG_MODEL("[MODEL] OpenSky meta JSON error\n");
    }
  }
  http.end();
  return "";
}

static String fetchFromHexDB(const String& icao24) {
  HTTPClient http;
  http.setTimeout(15000);

  String hex = icao24; hex.toLowerCase(); // HexDB expects lowercase
  String url = "https://hexdb.io/api/v1/aircraft/" + hex;
  DBG_HTTP("[HTTP] GET %s\n", url.c_str());
  http.begin(url);
  int code = http.GET();
  DBG_HTTP("[HTTP] <- %d (HexDB)\n", code);
  if (code == 200) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, http.getStream());
    http.end();
    if (!err) {
      if (!doc["error"].isNull()) { DBG_MODEL("[MODEL] HexDB error field present\n"); return ""; }

      String owners  = doc["RegisteredOwners"] | "";
      String opflag  = doc["OperatorFlagCode"] | "";
      String manuf   = doc["Manufacturer"]     | "";
      String type    = doc["Type"]             | "";
      String icao    = doc["ICAOTypeCode"]     | "";

      owners.trim(); opflag.trim(); manuf.trim(); type.trim(); icao.trim();

      String manufN = normalizeManufacturer(manuf);

      // Candidate order
      struct Cand { String a,b,c; };
      Cand seq[] = {
        { owners, manuf,  type },
        { owners, manuf,  icao },
        { owners, manufN, type },
        { owners, manufN, icao },
        { owners, "",     icao },

        { opflag, manuf,  type },
        { opflag, manuf,  icao },
        { opflag, manufN, type },
        { opflag, manufN, icao },

        { owners, manufN, ""   },
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

      auto fits = [&](const String& s){ return (int)s.length() <= 36; };

      for (const auto& c : seq) {
        String s = join3(c.a, c.b, c.c);
        if (s.length() && fits(s)) { DBG_MODEL("[MODEL] HexDB chose '%s'\n", s.c_str()); return s; }
      }

      String base = join3(opflag, manufN, icao);
      if (!base.length()) base = join3(opflag, "", icao);
      if (!base.length()) base = opflag;
      if (!base.length()) base = join3(owners, manufN, icao);
      if (!base.length()) base = icao;
      if (!base.length()) base = type;
      if (!base.length()) base = "Unknown";
      base = ellipsize(base, 36);
      DBG_MODEL("[MODEL] HexDB base '%s'\n", base.c_str());
      return base;
    } else {
      DBG_MODEL("[MODEL] HexDB JSON error\n");
    }
  } else {
    http.end();
  }
  return "";
}

static String fetchCallsignFromADSBOne(const String& icao24) {
  String hex = icao24; hex.toUpperCase();

  {
    String hit = getCachedCallsign(hex);
    if (hit.length() || gHexToCallsign.count(hex)) {
      DBG_ROUTE("[ROUTE] callsign cache %s -> '%s'\n", hex.c_str(), hit.c_str());
      return hit;
    }
  }

  if (!allowADSBCall()) return "";

  HTTPClient http;
  http.setTimeout(ADSB_TIMEOUT_MS);
  String url = "https://api.adsb.one/v2/hex/" + hex;
  DBG_HTTP("[HTTP] GET %s\n", url.c_str());
  http.begin(url);
  int code = http.GET();
  DBG_HTTP("[HTTP] <- %d (ADSB.one callsign)\n", code);
  if (code != 200) { http.end(); cacheCallsign(hex, ""); return ""; }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, http.getStream());
  http.end();
  if (err) { cacheCallsign(hex, ""); DBG_ROUTE("[ROUTE] callsign JSON error\n"); return ""; }

  JsonArray ac = doc["ac"].as<JsonArray>();
  if (ac.isNull() || ac.size() == 0) { cacheCallsign(hex, ""); DBG_ROUTE("[ROUTE] callsign empty array\n"); return ""; }

  String cs = (const char*)(ac[0]["flight"] | "");
  cs.trim();
  cacheCallsign(hex, cs);
  DBG_ROUTE("[ROUTE] callsign from ADSB.one %s -> '%s'\n", hex.c_str(), cs.c_str());
  return cs;
}

static String fetchModelFromADSBOneDesc(const String& icao24) {
  HTTPClient http;
  http.setTimeout(ADSB_TIMEOUT_MS);

  String hex = icao24; hex.toUpperCase();
  String url = "https://api.adsb.one/v2/hex/" + hex;
  DBG_HTTP("[HTTP] GET %s\n", url.c_str());
  http.begin(url);
  int code = http.GET();
  DBG_HTTP("[HTTP] <- %d (ADSB.one model)\n", code);
  if (code != 200) { http.end(); return ""; }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, http.getStream());
  http.end();
  if (err) { DBG_MODEL("[MODEL] ADSB1 JSON error\n"); return ""; }

  JsonArray ac = doc["ac"].as<JsonArray>();
  if (ac.isNull() || ac.size() == 0) return "";

  String model = (const char*)(ac[0]["desc"] | "");
  if (!model.length()) model = (const char*)(ac[0]["t"] | "");
  model.trim();
  DBG_MODEL("[MODEL] ADSB1 '%s'\n", model.c_str());
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
  if (os.length() && ds.length()) return os + "%" + ds;
  return "";
}

static String fetchRouteLabelForCallsign(String callsign) {
  callsign.trim();
  callsign.replace(" ", "");
  if (!callsign.length()) return "";

  {
    String hit = getCachedRoute(callsign);
    if (hit.length() || routeCacheHas(callsign)) {
      DBG_ROUTE("[ROUTE] route cache %s -> '%s'\n", callsign.c_str(), hit.c_str());
      return hit;
    }
  }

  if (!allowRouteCall()) return "";

  HTTPClient http;
  http.setTimeout(API_TIMEOUT_MS);
  String url = "https://api.adsbdb.com/v0/callsign/" + callsign;
  DBG_HTTP("[HTTP] GET %s\n", url.c_str());
  http.begin(url);
  int code = http.GET();
  DBG_HTTP("[HTTP] <- %d (ADSBdb)\n", code);
  if (code != 200) { http.end(); cacheRoute(callsign, ""); return ""; }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, http.getStream());
  http.end();
  if (err) { cacheRoute(callsign, ""); DBG_ROUTE("[ROUTE] ADSBdb JSON error\n"); return ""; }

  JsonObject fr = doc["response"]["flightroute"];
  String label = fr.isNull() ? "" : routeLabelFromFlightroute(fr);
  cacheRoute(callsign, label);
  DBG_ROUTE("[ROUTE] ADSBdb %s -> '%s'\n", callsign.c_str(), label.c_str());
  return label;
}

// NEW: airline-name fetcher (for last-ditch model fill)
static String fetchAirlineNameFromADSBdb(String callsign) {
  callsign.trim();
  callsign.replace(" ", "");
  if (!callsign.length()) return "";

  // cache first (positive or negative)
  {
    String hit = getCachedAirline(callsign);
    if (hit.length() || gCallsignToAirline.count(callsign)) {
      DBG_MODEL("[MODEL] airline cache %s -> '%s'\n", callsign.c_str(), hit.c_str());
      return hit;
    }
  }

  if (!allowRouteCall()) return ""; // reuse route token bucket

  HTTPClient http;
  http.setTimeout(API_TIMEOUT_MS);
  String url = "https://api.adsbdb.com/v0/callsign/" + callsign;
  DBG_HTTP("[HTTP] GET %s\n", url.c_str());
  http.begin(url);
  int code = http.GET();
  DBG_HTTP("[HTTP] <- %d (ADSBdb airline)\n", code);
  if (code != 200) { http.end(); cacheAirline(callsign, ""); return ""; }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, http.getStream());
  http.end();
  if (err) { cacheAirline(callsign, ""); DBG_MODEL("[MODEL] ADSBdb airline JSON error\n"); return ""; }

  String name = (const char*)(doc["response"]["flightroute"]["airline"]["name"] | "");
  name.trim();
  cacheAirline(callsign, name);
  if (name.length()) DBG_MODEL("[MODEL] ADSBdb airline '%s' for %s\n", name.c_str(), callsign.c_str());
  return name;
}

// ADSBdb aircraft endpoint: https://api.adsbdb.com/v0/aircraft/{icao24}
// Returns manufacturer, type, icao_type (uppercase)
static bool fetchAircraftFieldsFromADSBdb(const String& icao24,
                                          String& outManufacturer,
                                          String& outType,
                                          String& outIcaoType)
{
  // Reuse ADSBdb token bucket (same domain as callsign/route)
  if (!allowRouteCall()) return false;

  HTTPClient http;
  http.setTimeout(API_TIMEOUT_MS);
  String hexU = icao24; hexU.toUpperCase();
  String url = "https://api.adsbdb.com/v0/aircraft/" + hexU;
  DBG_HTTP("[HTTP] GET %s\n", url.c_str());
  http.begin(url);
  int code = http.GET();
  DBG_HTTP("[HTTP] <- %d (ADSBdb aircraft)\n", code);
  if (code != 200) { http.end(); return false; }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, http.getStream());
  http.end();
  if (err) {
    DBG_MODEL("[MODEL] ADSBdb aircraft JSON error\n");
    return false;
  }

  JsonObject ac = doc["response"]["aircraft"];
  if (ac.isNull()) return false;

  String manufacturer = (const char*)(ac["manufacturer"]   | "");
  String type         = (const char*)(ac["type"]           | "");
  String icaoType     = (const char*)(ac["icao_type"]      | "");

  manufacturer.trim(); type.trim(); icaoType.trim();
  outManufacturer = manufacturer;
  outType         = type;
  outIcaoType     = icaoType;

  DBG_MODEL("[MODEL] ADSBdb aircraft fields: manuf='%s' type='%s' icao='%s'\n",
            outManufacturer.c_str(), outType.c_str(), outIcaoType.c_str());
  return (outManufacturer.length() || outType.length() || outIcaoType.length());
}

static bool modelFetchAllowed() { return (int32_t)(gModelFailUntilMs - millis()) <= 0; }

static String bestFlightLabel(const String& icao24, const String& callsignIn) {
  String v = lookupCachedDisplayLabelByIcao(icao24);
  if (v.length()) { DBG_ROUTE("[ROUTE] reuse cached label for %s -> '%s'\n", icao24.c_str(), v.c_str()); return v; }

  String cs = callsignIn; cs.trim(); cs.replace(" ", ""); cs.toUpperCase();
  if (!cs.length()) { DBG_ROUTE("[ROUTE] no callsign for %s, using icao\n", icao24.c_str()); return icao24; }

  String hit = getCachedRoute(cs);
  if (hit.length() || routeCacheHas(cs)) {
    DBG_ROUTE("[ROUTE] route cache %s -> '%s'\n", cs.c_str(), hit.c_str());
    return hit;
  }
  String route = fetchRouteLabelForCallsign(cs);
  if (route.length()) {
    DBG_ROUTE("[ROUTE] ADSBdb %s -> '%s'\n", cs.c_str(), route.c_str());
    return route;
  }
  DBG_ROUTE("[ROUTE] fallback callsign %s\n", cs.c_str());
  return cs;
}

// ---------------- Public: aircraft model (with airline-name fallback) ----------------
String fetchAircraftModel(const String& icao24, OpenSkyAuthClient& auth, const String& callsignOpt) {
  // 0) Cached?
  String cached = lookupCachedModel(icao24);
  if (isMeaningfulModel(cached)) {
    DBG_MODEL("[MODEL] cache hit %s -> '%s'\n", icao24.c_str(), cached.c_str());
    return cached;
  }

  if (!modelFetchAllowed()) {
    DBG_MODEL("[MODEL] backoff active; returning cached/Unknown for %s\n", icao24.c_str());
    if (isMeaningfulModel(cached)) return cached;
    // still try cheap airline name in backoff mode
    if (callsignOpt.length()) {
      String cs = callsignOpt; cs.trim(); cs.replace(" ", ""); cs.toUpperCase();
      String airline = fetchAirlineNameFromADSBdb(cs);
      if (isMeaningfulModel(airline)) {
        addToCache(icao24, airline, "", "", -1, -1);
        return airline;
      }
    }
    return "Unknown";
  }

  // 1) Normal provider order (existing)
  String model = fetchFromHexDB(icao24);
  if (isMeaningfulModel(model)) { DBG_MODEL("[MODEL] win=HexDB   %s -> '%s'\n", icao24.c_str(), model.c_str()); goto WIN; }

  model = fetchFromOpenSkyMeta(icao24, auth);
  if (isMeaningfulModel(model)) { DBG_MODEL("[MODEL] win=OpenSky %s -> '%s'\n", icao24.c_str(), model.c_str()); goto WIN; }

  model = fetchFromPlaneSpotters(icao24);
  if (isMeaningfulModel(model)) { DBG_MODEL("[MODEL] win=PSpot   %s -> '%s'\n", icao24.c_str(), model.c_str()); goto WIN; }

  model = fetchModelFromADSBOneDesc(icao24);
  if (isMeaningfulModel(model)) { DBG_MODEL("[MODEL] win=ADSB1   %s -> '%s'\n", icao24.c_str(), model.c_str()); goto WIN; }

  // 2) NEW: Before pure airline fallback, try ADSBdb aircraft fields
  {
    String mfr, typ, icao;
    bool gotAircraft = fetchAircraftFieldsFromADSBdb(icao24, mfr, typ, icao);

    // We can also ask for airline (cheap) if callsign available
    String airline;
    if (callsignOpt.length()) {
      String cs = callsignOpt; cs.trim(); cs.replace(" ", ""); cs.toUpperCase();
      airline = fetchAirlineNameFromADSBdb(cs);
    }

    if (gotAircraft || airline.length()) {
      String composed = composeModelLabelPriority(airline, mfr, typ, icao, MAX_MODEL_CHARS);
      if (isMeaningfulModel(composed)) {
        DBG_MODEL("[MODEL] win=ADSBdb aircraft compose %s -> '%s'\n", icao24.c_str(), composed.c_str());
        addToCache(icao24, composed, "", "", -1, -1);
        return composed;
      }
    }
  }

  // 3) Last-ditch: airline-only (what you had before)
  if (callsignOpt.length()) {
    String cs = callsignOpt; cs.trim(); cs.replace(" ", ""); cs.toUpperCase();
    String airline = fetchAirlineNameFromADSBdb(cs);
    if (isMeaningfulModel(airline)) {
      DBG_MODEL("[MODEL] win=ADSBdb-airline %s -> '%s'\n", icao24.c_str(), airline.c_str());
      addToCache(icao24, airline, "", "", -1, -1);
      return airline;
    }
  }

  // 4) Everything failed → brief backoff and "Unknown"
  DBG_MODEL("[MODEL] all providers failed for %s -> 'Unknown'\n", icao24.c_str());
  gModelFailUntilMs = millis() + 15000UL;
  return "Unknown";

WIN:
  addToCache(icao24, model, "", "", -1, -1);
  return model;
}

// Re-run callsign/route lookups only for sparse rows.
// We throttle to a few per tick and respect token buckets & negative caches.
static void reenrichSparseEntries(int maxPerTick = 3) {
  struct Cand {
    String icao24;
    String label;     // current callsign/route label field in cache
    String country;
    float  distance;
    float  bearing;
  };

  // 1) Snapshot candidates quickly under mutex (no network here)
  Cand todo[8];
  int n = 0;

  if (!gCacheMutex || xSemaphoreTake(gCacheMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    for (int i = 0; i < MAX_CACHE_SIZE && n < (int)(sizeof(todo)/sizeof(todo[0])); ++i) {
      const auto& e = aircraftCache[i];
      if (e.icao24.length() == 0) continue;

      // “Sparse” if we have nothing in the callsign/route column,
      // or if it looks like a raw callsign (no AAA-BBB route).
      bool missing = (e.callsign.length() == 0);
      bool likelyRawCallsign = (!missing && e.callsign.indexOf('-') == -1);

      if (missing || likelyRawCallsign) {
        todo[n++] = Cand{
          e.icao24,
          e.callsign,
          e.country,
          e.distance,
          e.bearing
        };
      }
    }
    if (gCacheMutex) xSemaphoreGive(gCacheMutex);
  }

  if (n == 0) return;

  // 2) Process a limited number per tick (network allowed here)
  int done = 0;
  for (int i = 0; i < n && done < maxPerTick; ++i) {
    const auto& c = todo[i];

    // If we already have a route (AAA-BBB), skip
    if (c.label.length() && c.label.indexOf('-') != -1) continue;

    // Try to get/refresh callsign from ADSB.one
    String hexU = c.icao24; hexU.toUpperCase();
    String callsign = getCachedCallsign(hexU);
    if (!callsign.length()) {
      // May be throttled; if throttled, skip this candidate now
      if (!allowADSBCall()) continue;
      callsign = fetchCallsignFromADSBOne(hexU);  // caches hit/miss internally
    }

    // If still no callsign, nothing to do
    if (!callsign.length()) continue;

    // With callsign in hand, try to get route from ADSBdb
    String cs = callsign; cs.trim(); cs.replace(" ", ""); cs.toUpperCase();

    // Respect negative cache: if present but empty, routeCacheHas(cs) will be true
    String cachedRoute = getCachedRoute(cs);
    bool haveCached = routeCacheHas(cs);
    String label = cachedRoute;

    if (!haveCached) {
      // Maybe throttled; if throttled, skip for now
      if (!allowRouteCall()) continue;
      label = fetchRouteLabelForCallsign(cs);  // caches hit (10m) / miss (60s)
    }

    // Finalize what to display:
    // - Prefer route "AAA-BBB" if we got one,
    // - Else use the normalized callsign itself.
    String finalLabel = label.length() ? label : cs;

    // 3) Write back into cache (preserve model/country/dist/bearing)
    // We don't change model here; just update the callsign/route field.
    // addToCache merges fields when non-empty.
    addToCache(c.icao24, /*model*/"", finalLabel, c.country, c.distance, c.bearing);

    DBG_ROUTE("[RETRY] %s -> callsign='%s'  route='%s'  final='%s'\n",
              c.icao24.c_str(), callsign.c_str(), label.c_str(), finalLabel.c_str());

    ++done;
    yield();
  }
}


// ---------------- Main fetch ----------------
void fetchOpenSkyDataWithBoundingBox(float centerLat, float centerLon, int zoom, OpenSkyAuthClient& auth) {
  static bool isBusy = false;
  static bool lastHadAircraft = true;
  if (isBusy) return;
  isBusy = true;

  // prune small maps and old cache rows periodically
  pruneSmallMaps();
  static uint32_t lastCachePrune = 0;
  uint32_t now = millis();
  if ((uint32_t)(now - lastCachePrune) > 60000UL) {
    pruneCacheByAge(20UL * 60UL * 1000UL); // 20 minutes
    lastCachePrune = now;
  }

  if (!auth.ensureValidToken()) {
    isBusy = false;
    if (getDisplayMode() == LIVE_MODE) drawStatusScreen("Token fetch error");
    return;
  }

  const BoundingBox box = getBoundingBox(centerLat, centerLon, zoom);
  DBG_FETCH("[FETCH] bbox lat=[%.3f..%.3f] lon=[%.3f..%.3f] zoom=%d\n",
            box.south, box.north, box.west, box.east, zoom);

  String url; url.reserve(160);
  url  = "https://opensky-network.org/api/states/all?";
  url += "lamin=" + String(box.south, 5);
  url += "&lamax=" + String(box.north, 5);
  url += "&lomin=" + String(box.west, 5);
  url += "&lomax=" + String(box.east, 5);

  HTTPClient http;
  http.setTimeout(STATES_TIMEOUT_MS);
  http.useHTTP10(true);                        // robust path (avoids chunked issues)
  DBG_HTTP("[HTTP] GET %s\n", url.c_str());
  http.begin(url);
  http.addHeader("Authorization", "Bearer " + auth.getAccessToken());
  http.addHeader("Connection", "close");
  http.addHeader("Accept", "application/json");
  http.addHeader("Accept-Encoding", "identity");

  int httpCode = http.GET();
  DBG_HTTP("[HTTP] <- %d (OpenSky states)\n", httpCode);
  if (httpCode != 200) {
    Serial.printf("[fetch] HTTP %d from states/all\n", httpCode);
    http.end();
    isBusy = false;
    return;
  }

  // Parse JSON (large)
  JsonDocument doc; // adjust to your RAM; or use filters
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
  DBG_FETCH("[FETCH] parsed states: %d\n", totalAircraft);
  dbg_heap("after OpenSky parse");

  // guard: too many rows to render
  if (totalAircraft > MAX_CACHE_SIZE) {
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
      const char* csSrc = "OpenSky";
      callsign.trim();
      if (!callsign.length()) {
        String hexU = icao24; hexU.toUpperCase();
        callsign = getCachedCallsign(hexU);
        if (!callsign.length()) callsign = fetchCallsignFromADSBOne(hexU);
        csSrc = "cache/ADSB1";
      }

      float dist = haversineDistance(centerLat, centerLon, lat, lon);
      float brng = calculateBearing(centerLat, centerLon, lat, lon);

      // Get model with airline-name fallback (do NOT degrade on later cycles)
      String model = fetchAircraftModel(icao24, auth, callsign);
      if (!isMeaningfulModel(model)) {
        String prev = lookupCachedModel(icao24);
        if (isMeaningfulModel(prev)) model = prev;
      }

      String flightLabel = bestFlightLabel(icao24, callsign);

      DBG_FETCH("[ACFT] %s  cs='%s'(%s)  dist=%.1fkm  brg=%.0f°\n",
                icao24.c_str(), callsign.c_str(), csSrc, dist, brng);

      addToCache(icao24, model, flightLabel, country, dist, brng);

      // mark new row active + freshness bump
      if (!gCacheMutex || xSemaphoreTake(gCacheMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        for (int i = 0; i < MAX_CACHE_SIZE; i++) {
          if (aircraftCache[i].icao24 == icao24) {
            aircraftCache[i].active = true;
            aircraftCache[i].lastSeenMs = millis();
            if (aircraftCache[i].seenCount < 0xFFFF) aircraftCache[i].seenCount++;
            break;
          }
        }
        if (gCacheMutex) xSemaphoreGive(gCacheMutex);
      } else {
        touchIcao(icao24);
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
  reenrichSparseEntries(/*maxPerTick=*/3);
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
