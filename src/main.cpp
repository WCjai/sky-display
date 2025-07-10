// main.cpp
#include <Arduino.h>
#include <WiFi.h>
#include <nvs_flash.h>

#include "config.h"
#include "display.h"
#include "fetch.h"
#include "OpenSkyAuthClient.h"

OpenSkyAuthClient* pAuthClient = nullptr;

#define BUTTON_PIN 2

void setup() {
  Serial.begin(115200);

  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    err = nvs_flash_init();
  }
  ESP_ERROR_CHECK(err);

  pinMode(BUTTON_PIN, INPUT_PULLUP);

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
  fetchOpenSkyDataWithBoundingBox(HOME_LAT, HOME_LON, ZOOM, *pAuthClient);
}

void loop() {
  static unsigned long lastFetch = 0;

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

  if (millis() - lastFetch > 15000) {
    lastFetch = millis();
    fetchOpenSkyDataWithBoundingBox(HOME_LAT, HOME_LON, ZOOM, *pAuthClient);
    printAircraftCacheSorted();
  }
}
