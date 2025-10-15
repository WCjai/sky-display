#include "cache.h"

// ===== DEBUG SWITCHES (set to 1 to enable) =====
#define DEBUG_CACHE   1
#define DEBUG_HEAP    0

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

#if DEBUG_CACHE
  #define DBG_CACHE(...)    do{ Serial.printf(__VA_ARGS__); }while(0)
#else
  #define DBG_CACHE(...)    do{}while(0)
#endif

// -------- storage --------
AircraftCacheEntry aircraftCache[MAX_CACHE_SIZE];
int cacheIndex = 0;

// -------- mutex --------
SemaphoreHandle_t gCacheMutex = nullptr;
void initCacheMutex() {
  if (!gCacheMutex) gCacheMutex = xSemaphoreCreateMutex();
}

// -------- local sanitation (bounds + ASCII printable only) --------
static String sanitize(const String& in, size_t maxLen) {
  String out;
  out.reserve(min((size_t)in.length(), maxLen));
  for (size_t i = 0; i < in.length() && out.length() < maxLen; ++i) {
    char c = in[i];
    if (c == '\r' || c == '\n' || c == '\t') c = ' ';
    if (c >= 32 && c <= 126) out += c;
  }
  out.trim();
  return out;
}

static inline bool isMeaningfulModel(const String& sIn) {
  String s = sIn; 
  s.trim();
  if (!s.length()) return false;
  if (s.equalsIgnoreCase("unknown")) return false;
  if (s.equalsIgnoreCase("null"))    return false;
  return true;
}

// 8-point compass with degree prefix e.g. "17N " (for Serial only)
String getCompassDirection(float bearing) {
  static const char* directions[] = {"N ","NE","E ","SE","S ","SW","W ","NW","N "};
  int index = (int)lroundf(bearing / 45.0f);
  if (index < 0) index = 0;
  if (index > 8) index = 8;
  char buf[12];
  snprintf(buf, sizeof(buf), "%d%s", (int)bearing, directions[index]);
  return String(buf);
}

String lookupCachedModel(const String& icao24) {
  const String sIcao = sanitize(icao24, 8);
  if (sIcao.isEmpty()) return "";

  String model = "";
  bool took = (!gCacheMutex) || (xSemaphoreTake(gCacheMutex, pdMS_TO_TICKS(150)) == pdTRUE);
  if (took) {
    for (int i = 0; i < MAX_CACHE_SIZE; ++i) {
      if (aircraftCache[i].icao24 == sIcao) { model = aircraftCache[i].model; break; }
    }
    if (gCacheMutex) xSemaphoreGive(gCacheMutex);
  }
  return model;
}

// ---------- mark “seen” without changing strings ----------
void touchIcao(const String& icao24) {
  const String sIcao = sanitize(icao24, 8);
  if (sIcao.isEmpty()) return;

  bool took = (!gCacheMutex) || (xSemaphoreTake(gCacheMutex, pdMS_TO_TICKS(120)) == pdTRUE);
  if (!took) return;

  for (int i = 0; i < MAX_CACHE_SIZE; ++i) {
    if (aircraftCache[i].icao24 == sIcao) {
      aircraftCache[i].lastSeenMs = millis();
      if (aircraftCache[i].seenCount < 0xFFFF) aircraftCache[i].seenCount++;
      DBG_CACHE("[CACHE] touch %s  seen=%u\n", sIcao.c_str(), aircraftCache[i].seenCount);
      break;
    }
  }
  if (gCacheMutex) xSemaphoreGive(gCacheMutex);
}

// ---------- prune entries not seen within maxAgeMs ----------
void pruneCacheByAge(uint32_t maxAgeMs) {
  const uint32_t now = millis();
  bool took = (!gCacheMutex) || (xSemaphoreTake(gCacheMutex, pdMS_TO_TICKS(200)) == pdTRUE);
  if (!took) return;

  for (int i = 0; i < MAX_CACHE_SIZE; ++i) {
    auto &e = aircraftCache[i];
    if (e.icao24.isEmpty()) continue;
    if (e.lastSeenMs == 0)   continue;
    if ((uint32_t)(now - e.lastSeenMs) > maxAgeMs) {
      DBG_CACHE("[CACHE] prune age>=%lu ms  %s  model='%s' cs='%s'\n",
        (unsigned long)maxAgeMs,
        e.icao24.c_str(), e.model.c_str(), e.callsign.c_str());
      e = {};
    }
  }
  if (gCacheMutex) xSemaphoreGive(gCacheMutex);
}

void printAircraftCacheSorted() {
  struct Row {
    String model, callsign, icao24;
    float  distance, bearing;
  };
  Row rows[MAX_CACHE_SIZE];
  int n = 0;

  bool took = (!gCacheMutex) || (xSemaphoreTake(gCacheMutex, pdMS_TO_TICKS(200)) == pdTRUE);
  if (took) {
    for (int i = 0; i < MAX_CACHE_SIZE; ++i) {
      const auto& e = aircraftCache[i];
      if (!e.icao24.isEmpty()) {
        rows[n++] = Row{ e.model, e.callsign, e.icao24, e.distance, e.bearing };
      }
    }
    if (gCacheMutex) xSemaphoreGive(gCacheMutex);
  }

  auto key = [](float d){ return (d < 0.0f) ? 1e9f : d; };
  for (int i = 1; i < n; ++i) {
    Row krow = rows[i];
    float k  = key(krow.distance);
    int j = i - 1;
    while (j >= 0 && key(rows[j].distance) > k) { rows[j+1] = rows[j]; --j; }
    rows[j+1] = krow;
  }

  Serial.println(F("---- Aircraft Cache (sorted by distance) ----"));
  for (int i = 0; i < n; ++i) {
    String dir = getCompassDirection(rows[i].bearing);
    const String& cs = rows[i].callsign.length() ? rows[i].callsign : rows[i].icao24;
    if (rows[i].distance >= 0.0f) {
      Serial.printf("%s | %-8s | %-36s | %7.1f km | %s\n",
        rows[i].icao24.c_str(), cs.c_str(), rows[i].model.c_str(),
        rows[i].distance, dir.c_str());
    } else {
      Serial.printf("%s | %-8s | %-36s |      --   | %s\n",
        rows[i].icao24.c_str(), cs.c_str(), rows[i].model.c_str(),
        dir.c_str());
    }
  }
  Serial.println(F("---------------------------------------------"));
}

// ---------- Hardened: never-degrade writes + smarter victim ----------
void addToCache(const String& icao24,
                const String& model,
                const String& callsign,
                const String& country,
                float distance,
                float bearing) {
  const String sIcao  = sanitize(icao24,  8);
  const String sModel = sanitize(model,   46);
  const String sCall  = sanitize(callsign, 8);
  const String sCntry = sanitize(country, 31);

  if (sIcao.isEmpty()) return;

  const uint32_t now = millis();

  auto meaningful = [](const String& s){
    String t = s; t.trim();
    return t.length() > 0 && !t.equalsIgnoreCase("unknown") && !t.equalsIgnoreCase("null");
  };

  bool took = (!gCacheMutex) || (xSemaphoreTake(gCacheMutex, pdMS_TO_TICKS(150)) == pdTRUE);
  if (!took) return;

  // 1) Update path (never degrade)
  for (int i = 0; i < MAX_CACHE_SIZE; ++i) {
    if (aircraftCache[i].icao24 == sIcao) {
      DBG_CACHE("[CACHE] update %s  dist=%.1f brg=%.1f\n",
        sIcao.c_str(), distance, bearing);

      if (meaningful(sModel))  aircraftCache[i].model    = sModel;
      if (sCall.length())      aircraftCache[i].callsign = sCall;
      if (sCntry.length())     aircraftCache[i].country  = sCntry;
      if (distance >= 0.0f)    aircraftCache[i].distance = distance;
      if (bearing  >= 0.0f)    aircraftCache[i].bearing  = bearing;

      aircraftCache[i].lastSeenMs = now;
      if (aircraftCache[i].seenCount < 0xFFFF) aircraftCache[i].seenCount++;

      DBG_CACHE("        -> model='%s' cs='%s' country='%s'\n",
        aircraftCache[i].model.c_str(),
        aircraftCache[i].callsign.c_str(),
        aircraftCache[i].country.c_str());

      if (gCacheMutex) xSemaphoreGive(gCacheMutex);
      return;
    }
  }

  // 2a) prefer an empty slot
  int idx = -1;
  for (int i = 0; i < MAX_CACHE_SIZE; ++i) {
    if (aircraftCache[i].icao24.isEmpty()) { idx = i; break; }
  }

  // 2b) otherwise pick the oldest inactive as victim; if all active, use ring
  if (idx < 0) {
    uint32_t oldestAge = 0;
    int oldestIdx = -1;
    for (int i = 0; i < MAX_CACHE_SIZE; ++i) {
      if (aircraftCache[i].active) continue;
      uint32_t age = now - aircraftCache[i].lastSeenMs; // 0 means “really old” as well
      if (oldestIdx < 0 || age > oldestAge) { oldestAge = age; oldestIdx = i; }
    }
    idx = (oldestIdx >= 0) ? oldestIdx : cacheIndex;
    DBG_CACHE("[CACHE] insert victim idx=%d (allActive=%s)\n", idx, (oldestIdx<0?"yes":"no"));
    cacheIndex = (cacheIndex + 1) % MAX_CACHE_SIZE;
  } else {
    DBG_CACHE("[CACHE] insert into empty idx=%d\n", idx);
  }

  // never store empty/unknown model as a downgrade; set "Unknown" only if nothing meaningful
  String storeModel = meaningful(sModel) ? sModel : String(F("Unknown"));

  aircraftCache[idx].icao24   = sIcao;
  aircraftCache[idx].model    = storeModel;
  aircraftCache[idx].callsign = sCall;
  aircraftCache[idx].country  = sCntry;
  aircraftCache[idx].distance = distance;
  aircraftCache[idx].bearing  = bearing;
  aircraftCache[idx].active   = false;
  aircraftCache[idx].lastSeenMs = now;
  aircraftCache[idx].seenCount  = 1;

  DBG_CACHE("[CACHE] add %s  model='%s' cs='%s' dist=%.1f brg=%.1f\n",
    sIcao.c_str(), aircraftCache[idx].model.c_str(),
    aircraftCache[idx].callsign.c_str(), distance, bearing);

  if (gCacheMutex) xSemaphoreGive(gCacheMutex);
}
