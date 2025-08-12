// main.cpp
#include <Arduino.h>
#include <WiFi.h>
#include <nvs_flash.h>

#include "config.h"
#include "display.h"
#include "fetch.h"
#include "OpenSkyAuthClient.h"

OpenSkyAuthClient* pAuthClient = nullptr;

// --- Buttons ---
// BUTTON_PIN (2) already used for config at boot; we also use it to exit HOLD at runtime.
#define BUTTON_PIN       2    // EXIT HOLD (short press during runtime), also long-press at boot for config
#define UP_BUTTON_PIN    32   // page up (enter HOLD on first press)
#define DOWN_BUTTON_PIN  33   // page down (enter HOLD on first press)

// --- Simple debounce ---
const unsigned long DEBOUNCE_MS = 80;
static bool upLast = HIGH, downLast = HIGH, exitLast = HIGH;
static unsigned long upLastChange = 0, downLastChange = 0, exitLastChange = 0;

volatile bool gHoldRequested = false;  // set by UP/DOWN ISR to preempt live drawing

// Simple ISRs: just set a flag (no heavy work here)
void IRAM_ATTR onUpPress()   { gHoldRequested = true; }
void IRAM_ATTR onDownPress() { gHoldRequested = true; }


static bool readButtonFalling(int pin, bool& last, unsigned long& lastChange) {
  bool now = digitalRead(pin);
  unsigned long t = millis();
  if (now != last && (t - lastChange) > DEBOUNCE_MS) {
    last = now;
    lastChange = t;
    return (now == LOW); // INPUT_PULLUP -> pressed is LOW
  }
  return false;
}

void setup() {
  Serial.begin(115200);

  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    err = nvs_flash_init();
  }
  ESP_ERROR_CHECK(err);

  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(UP_BUTTON_PIN, INPUT_PULLUP);
  pinMode(DOWN_BUTTON_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(UP_BUTTON_PIN),   onUpPress,   FALLING);
  attachInterrupt(digitalPinToInterrupt(DOWN_BUTTON_PIN), onDownPress, FALLING);


  epd.Init();
  epd.Clear();
  full_paint = Paint(full_image, 400, 300);

  // --- Boot-time config mode (long press at boot) ---
  if (digitalRead(BUTTON_PIN) == LOW) {
    drawStatusScreenwithline(
      "Entered Configuration mode",
      "-Connect to \"WC_Sky_display\" WiFi",
      "-Enter \"192.168.4.1\" in browser",
      " as URL, form connected device",
      "-Configure the parameters and save",
      "-Display will auto-refresh and boot"
    );
    startConfigMode();
  }

  loadConfig();

  if (WIFI_SSID.isEmpty() || WIFI_PASSWORD.isEmpty()) {
    drawStatusScreen("No WiFi creds, entering config...");
    startConfigMode();
  }

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID.c_str(), WIFI_PASSWORD.c_str());

  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 10000) {
    delay(500);
    Serial.print(".");
  }

  if (WiFi.status() != WL_CONNECTED) {
    drawStatusScreen("Wi-Fi failed. Reboot to retry.");
    delay(3000);
    ESP.restart();
  }

  drawStatusScreen("WiFi connected");
  pAuthClient = new OpenSkyAuthClient(CLIENT_ID.c_str(), CLIENT_SECRET.c_str());
  fetchOpenSkyDataWithBoundingBox(HOME_LAT, HOME_LON, ZOOM, *pAuthClient);
}

void loop() {
  static unsigned long lastFetch = 0;
  static uint32_t lastPreemptAt = 0;

  if (gHoldRequested) {
    gHoldRequested = false;

    // simple cooldown to avoid multiple ISR bounces forcing page 1 repeatedly
    uint32_t now = millis();
    if (now - lastPreemptAt > 120) {
      lastPreemptAt = now;

      // Enter HOLD *without drawing*. Let the button handler decide what to draw.
      if (getDisplayMode() != HOLD_MODE) {
        setDisplayMode(HOLD_MODE);   // IMPORTANT: setDisplayMode(HOLD_MODE) must NOT draw
      }
    }
    // Do NOT call redrawHoldPageFull() here.
  }

  // UP: enter HOLD & show page 1; while in HOLD: previous page
  if (readButtonFalling(UP_BUTTON_PIN, upLast, upLastChange)) {
    if (getDisplayMode() != HOLD_MODE) {
      setDisplayMode(HOLD_MODE);    // no draw inside
      redrawHoldPageFull();         // ONE full refresh (page 1)
    } else {
      pageDown();                   // ONE full refresh (page -1)
    }
  }

  // DOWN: enter HOLD & go to page 2; while in HOLD: next page
  if (readButtonFalling(DOWN_BUTTON_PIN, downLast, downLastChange)) {
    if (getDisplayMode() != HOLD_MODE) {
      setDisplayMode(HOLD_MODE);    // no draw inside
      pageUp();                     // ONE full refresh (page 2)
    } else {
      pageUp();                     // ONE full refresh (page +1)
    }
  }

  // EXIT to LIVE (pin 2)
  if (readButtonFalling(BUTTON_PIN, exitLast, exitLastChange)) {
    setDisplayMode(LIVE_MODE);      // draws once (LIVE)
  }


  // --- Connection heal ---
  if (WiFi.status() != WL_CONNECTED) {
    drawStatusScreen("WiFi lost! Reconnecting...");
    WiFi.disconnect();
    WiFi.begin(WIFI_SSID.c_str(), WIFI_PASSWORD.c_str());
    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 10000) {
      delay(500);
    }
    if (WiFi.status() == WL_CONNECTED) {
      drawStatusScreen("WiFi reconnected");
      delay(1000);
    } else {
      return;
    }
  }

  // --- Periodic fetch (keeps cache fresh even in HOLD) ---
  if (millis() - lastFetch > 25000) {
    lastFetch = millis();
    fetchOpenSkyDataWithBoundingBox(HOME_LAT, HOME_LON, ZOOM, *pAuthClient);
    printAircraftCacheSorted();
  }
}
