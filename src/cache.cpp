#include "cache.h"

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
    // keep basic printable ASCII; collapse tabs/newlines to space
    if (c == '\r' || c == '\n' || c == '\t') c = ' ';
    if (c >= 32 && c <= 126) out += c;
  }
  out.trim();
  return out;
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

// -------- core ops (all thread-safe) --------

// void addToCache(const String& icao24,
//                 const String& model,
//                 const String& callsign,
//                 const String& country,
//                 float distance,
//                 float bearing) {
//   // sanitize + bound all text up front
//   const String sIcao  = sanitize(icao24,   8);   // hex is 6–8 chars
//   const String sModel = sanitize(model,   46);   // fits in 48 chars UI buffer
//   const String sCall  = sanitize(callsign, 8);   // UI shows 7 + NUL
//   const String sCntry = sanitize(country, 31);   // fits in 32 chars UI buffer

//   if (sIcao.isEmpty()) return;

//   bool took = (!gCacheMutex) || (xSemaphoreTake(gCacheMutex, pdMS_TO_TICKS(150)) == pdTRUE);
//   if (!took) return;

//   // 1) update if already present
//   for (int i = 0; i < MAX_CACHE_SIZE; ++i) {
//     if (aircraftCache[i].icao24 == sIcao) {
//       if (sModel.length())   aircraftCache[i].model    = sModel;
//       if (callsign.length()) aircraftCache[i].callsign = sCall;
//       if (country.length())  aircraftCache[i].country  = sCntry;
//       if (distance >= 0.0f)  aircraftCache[i].distance = distance;
//       if (bearing  >= 0.0f)  aircraftCache[i].bearing  = bearing;
//       // don’t set active here; fetch loop controls it each cycle
//       if (gCacheMutex) xSemaphoreGive(gCacheMutex);
//       return;
//     }
//   }

//   // 2) insert at ring head
//   int idx = cacheIndex;
//   cacheIndex = (cacheIndex + 1) % MAX_CACHE_SIZE;

//   aircraftCache[idx].icao24   = sIcao;
//   aircraftCache[idx].model    = sModel.length() ? sModel : String("Unknown");
//   aircraftCache[idx].callsign = sCall;
//   aircraftCache[idx].country  = sCntry;
//   aircraftCache[idx].distance = distance;
//   aircraftCache[idx].bearing  = bearing;
//   // active flag is set by fetch when this ICAO is seen in the latest scan

//   if (gCacheMutex) xSemaphoreGive(gCacheMutex);
// }

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

  // insertion sort by distance; unknown (-1) goes last
  auto key = [](float d){ return (d < 0.0f) ? 1e9f : d; }; // sentinel, avoids <float.h>
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



void addToCache(const String& icao24,
                const String& model,
                const String& callsign,
                const String& country,
                float distance,
                float bearing) {
  // sanitize + bound all text up front
  const String sIcao  = sanitize(icao24,  8);  // hex is 6–8 chars
  const String sModel = sanitize(model,   46); // fits UI buffer
  const String sCall  = sanitize(callsign, 8); // UI shows 7 + NUL
  const String sCntry = sanitize(country, 31); // fits UI buffer

  if (sIcao.isEmpty()) return;

  bool took = (!gCacheMutex) || (xSemaphoreTake(gCacheMutex, pdMS_TO_TICKS(150)) == pdTRUE);
  if (!took) return;

  // 1) update if already present
  for (int i = 0; i < MAX_CACHE_SIZE; ++i) {
    if (aircraftCache[i].icao24 == sIcao) {
      if (sModel.length())  aircraftCache[i].model    = sModel;
      if (sCall.length())   aircraftCache[i].callsign = sCall;   // <-- was callsign.length()
      if (sCntry.length())  aircraftCache[i].country  = sCntry;  // <-- was country.length()
      if (distance >= 0.0f) aircraftCache[i].distance = distance;
      if (bearing  >= 0.0f) aircraftCache[i].bearing  = bearing;
      // 'active' is still managed by the fetch loop
      if (gCacheMutex) xSemaphoreGive(gCacheMutex);
      return;
    }
  }

  // 2a) try to insert into an empty slot first (optional, avoids early overwrite)
  int idx = -1;
  for (int i = 0; i < MAX_CACHE_SIZE; ++i) {
    if (aircraftCache[i].icao24.isEmpty()) { idx = i; break; }
  }

  // 2b) otherwise insert at ring head
  if (idx < 0) {
    idx = cacheIndex;
    cacheIndex = (cacheIndex + 1) % MAX_CACHE_SIZE;
  }

  aircraftCache[idx].icao24   = sIcao;
  aircraftCache[idx].model    = sModel.length() ? sModel : String(F("Unknown"));
  aircraftCache[idx].callsign = sCall;
  aircraftCache[idx].country  = sCntry;
  aircraftCache[idx].distance = distance;
  aircraftCache[idx].bearing  = bearing;
  // active flag is set by fetch when this ICAO is seen in the latest scan

  if (gCacheMutex) xSemaphoreGive(gCacheMutex);
}
