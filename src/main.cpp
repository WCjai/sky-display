// main.cpp
#include <Arduino.h>
#include <WiFi.h>
#include <nvs_flash.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "config.h"
#include "display.h"
#include "fetch.h"
#include "cache.h"
#include "OpenSkyAuthClient.h"

OpenSkyAuthClient* pAuthClient = nullptr;

#define BUTTON_PIN       2
#define UP_BUTTON_PIN    32
#define DOWN_BUTTON_PIN  33

const unsigned long DEBOUNCE_MS = 80;
static bool upLast = HIGH, downLast = HIGH, exitLast = HIGH;
static unsigned long upLastChange = 0, downLastChange = 0, exitLastChange = 0;

volatile bool gHoldRequested = false;

void IRAM_ATTR onUpPress()   { gHoldRequested = true; }
void IRAM_ATTR onDownPress() { gHoldRequested = true; }

static bool readButtonFalling(int pin, bool& last, unsigned long& lastChange) {
  bool now = digitalRead(pin);
  unsigned long t = millis();
  if (now != last && (t - lastChange) > DEBOUNCE_MS) {
    last = now;
    lastChange = t;
    return (now == LOW);
  }
  return false;
}

// ----------------- FreeRTOS fetch task -----------------
TaskHandle_t gFetchTaskHandle = nullptr;

void fetchTask(void* pv) {
  const TickType_t kDelay = pdMS_TO_TICKS(25000);
  for (;;) {
    if (WiFi.status() == WL_CONNECTED && pAuthClient) {
      fetchOpenSkyDataWithBoundingBox(HOME_LAT, HOME_LON, ZOOM, *pAuthClient);
      printAircraftCacheSorted(); // optional
    }
    vTaskDelay(kDelay);
  }
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

  // Create cache mutex BEFORE any fetch
  initCacheMutex();
  initDisplayMutex();
  

  // Optional initial fetch
  fetchOpenSkyDataWithBoundingBox(HOME_LAT, HOME_LON, ZOOM, *pAuthClient);

  // ↑↑ Increased stack from 8192 → 16384 bytes ↑↑
  xTaskCreatePinnedToCore(
    fetchTask, "fetchTask", 16384, nullptr, 2, &gFetchTaskHandle, 0
  );
}

void loop() {
  static uint32_t lastPreemptAt = 0;

  if (gHoldRequested) {
    gHoldRequested = false;
    uint32_t now = millis();
    if (now - lastPreemptAt > 120) {
      lastPreemptAt = now;
      if (getDisplayMode() != HOLD_MODE) setDisplayMode(HOLD_MODE);
    }
  }

  // UP
  if (readButtonFalling(UP_BUTTON_PIN, upLast, upLastChange)) {
    if (getDisplayMode() != HOLD_MODE) {
      setDisplayMode(HOLD_MODE);   // now this ALSO draws page 1
    } else {
      pageDown();                  // previous page (does partial draw inside)
    }
  }

  // DOWN
  if (readButtonFalling(DOWN_BUTTON_PIN, downLast, downLastChange)) {
    if (getDisplayMode() != HOLD_MODE) {
      setDisplayMode(HOLD_MODE);   // enters HOLD and draws page 1
      pageUp();                    // go to page 2 right away if you prefer
    } else {
      pageUp();                    // next page
    }
  }

  // EXIT to LIVE
  if (readButtonFalling(BUTTON_PIN, exitLast, exitLastChange)) {
    setDisplayMode(LIVE_MODE);     // draws immediately
  }

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

  delay(1);
}
