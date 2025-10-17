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
#include "imagedata.h"
// ----------------- Pins -----------------
#define BUTTON_PIN            2   // EXIT to LIVE
#define UP_BUTTON_PIN        33   // PAGE DOWN (prev)
#define DOWN_BUTTON_PIN      32   // PAGE UP (next)
#define EPD_ACTIVITY_LED_PIN 25

// ----------------- Globals -----------------
OpenSkyAuthClient* pAuthClient = nullptr;

// tiny flags set by ISR, consumed in loop()
volatile bool gHoldRequested = false;
volatile bool gUpEdge = false;
volatile bool gDownEdge = false;
volatile bool gExitEdge = false;

// ----------------- Debounce helpers -----------------
static const unsigned long DEBOUNCE_MS = 80;
static bool upLast = HIGH, downLast = HIGH, exitLast = HIGH;
static unsigned long upLastChange = 0, downLastChange = 0, exitLastChange = 0;

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

// ----------------- ISRs (IRAM, no heavy work) -----------------
void IRAM_ATTR onUpPress()   { gUpEdge   = true; }
void IRAM_ATTR onDownPress() { gDownEdge = true; }
void IRAM_ATTR onExitPress() { gExitEdge = true; }

// ----------------- FreeRTOS fetch task -----------------
TaskHandle_t gFetchTaskHandle = nullptr;

static void fetchTask(void* pv) {
  const TickType_t kDelay = pdMS_TO_TICKS(25000);  // poll cadence
  for (;;) {
    if (WiFi.status() == WL_CONNECTED && pAuthClient) {
      digitalWrite(EPD_ACTIVITY_LED_PIN, HIGH);
      fetchOpenSkyDataWithBoundingBox(HOME_LAT, HOME_LON, ZOOM, *pAuthClient);
      printAircraftCacheSorted(); // optional serial dump
      digitalWrite(EPD_ACTIVITY_LED_PIN, LOW);
    }
    vTaskDelay(kDelay);
  }
}

// ----------------- WiFi helpers -----------------
static bool connectWiFi(unsigned long timeoutMs = 10000) {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID.c_str(), WIFI_PASSWORD.c_str());

  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - t0) < timeoutMs) {
    delay(250);
    Serial.print('.');
  }
  Serial.println();
  return WiFi.status() == WL_CONNECTED;
}

// ----------------- Arduino setup/loop -----------------
void setup() {
  Serial.begin(115200);

  // NVS (for config portal etc.)
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    err = nvs_flash_init();
  }
  ESP_ERROR_CHECK(err);

  // IO
  pinMode(BUTTON_PIN,       INPUT_PULLUP);
  pinMode(UP_BUTTON_PIN,    INPUT_PULLUP);
  pinMode(DOWN_BUTTON_PIN,  INPUT_PULLUP);
  pinMode(EPD_ACTIVITY_LED_PIN, OUTPUT);
  digitalWrite(EPD_ACTIVITY_LED_PIN, LOW);

  // interrupts (edges only set flags)
  attachInterrupt(digitalPinToInterrupt(UP_BUTTON_PIN),   onUpPress,   FALLING);
  attachInterrupt(digitalPinToInterrupt(DOWN_BUTTON_PIN), onDownPress, FALLING);
  attachInterrupt(digitalPinToInterrupt(BUTTON_PIN),      onExitPress, FALLING);

  // EPD init — base screen will be managed by display.cpp
  epd.Init();
  epd.Clear();
  full_paint = Paint(full_image, 400, 300);

  // Config portal on boot-hold
  if (digitalRead(BUTTON_PIN) == LOW) {

  /* This displays an image */
    epd.Init();
    epd.Display(gImage_atc);
    delay(1000);
    startConfigMode(); // blocks until saved / timeout
  }

  // Load config
  loadConfig();
  if (WIFI_SSID.isEmpty() || WIFI_PASSWORD.isEmpty()) {
    drawStatusScreen("No WiFi creds, entering config...");
    startConfigMode();
  }

  //drawStatusScreen("Connecting Wi-Fi...");
  if (!connectWiFi()) {
    drawStatusScreen("Wi-Fi failed. Reboot to retry.");
    delay(3000);
    ESP.restart();
  }
  //drawStatusScreen("Wi-Fi connected");
  epd.Display(gImage_alive);

  // Auth client (token fetch deferred to fetcher)
  pAuthClient = new OpenSkyAuthClient(CLIENT_ID.c_str(), CLIENT_SECRET.c_str());

  // Create cache & display mutexes BEFORE any fetch/draw
  initCacheMutex();
  initDisplayMutex();
// epd.Init();
// epd.Clear();
// full_paint = Paint(full_image, 400, 300);

// Show a dummy live page at boot for 2s, then carry on
//debugShowDummyLiveBoot();
//debugShowDummyHoldBoot();
//drawHoldPagePartial();
delay(2000);
  // Start fetch task with larger stack (avoid canary in WiFi/json work)
  xTaskCreatePinnedToCore(
    fetchTask,
    "fetchTask",
    16384,          // stack words (~16 KB)
    nullptr,
    2,              // prio
    &gFetchTaskHandle,
    0               // run on core 0 (keep UI loop on core 1)
  );

  // small boot notice
  //drawStatusScreen("Starting...");
  delay(400);
}

void loop() {
  // Convert ISR edges into debounced actions
  if (gUpEdge)   { gUpEdge = false;   gHoldRequested = true; }
  if (gDownEdge) { gDownEdge = false; gHoldRequested = true; }
  if (gExitEdge) { gExitEdge = false; /* handled in poll below */ }

  // Minimal preemption guard for HOLD
  static uint32_t lastPreemptAt = 0;
  if (gHoldRequested) {
    gHoldRequested = false;
    uint32_t now = millis();
    if (now - lastPreemptAt > 120) {
      lastPreemptAt = now;
      if (getDisplayMode() != HOLD_MODE) setDisplayMode(HOLD_MODE);
    }
  }

  // ---- Button polling with debounce (safe, no heap use) ----
  // UP: go HOLD (if not) else previous page
  if (readButtonFalling(UP_BUTTON_PIN, upLast, upLastChange)) {
    if (getDisplayMode() != HOLD_MODE) {
      setDisplayMode(HOLD_MODE);
    } else {
      pageDown();  // previous page
    }
  }

  // DOWN: go HOLD (if not) else next page
  if (readButtonFalling(DOWN_BUTTON_PIN, downLast, downLastChange)) {
    if (getDisplayMode() != HOLD_MODE) {
      setDisplayMode(HOLD_MODE);
      pageUp();    // jump to next immediately if you like
    } else {
      pageUp();    // next page
    }
  }

  // EXIT: back to LIVE
  if (readButtonFalling(BUTTON_PIN, exitLast, exitLastChange)) {
    setDisplayMode(LIVE_MODE);
  }

  // ---- Wi-Fi keepalive (non-blocking) ----
  if (WiFi.status() != WL_CONNECTED) {
    drawStatusScreen("Wi-Fi lost! Reconnecting...");
    WiFi.disconnect();
    if (connectWiFi()) {
      drawStatusScreen("Wi-Fi reconnected");
      delay(800);
    } else {
      // leave loop early, let next tick retry
      delay(5);
      return;
    }
  }

  // breath
  delay(1);
}
