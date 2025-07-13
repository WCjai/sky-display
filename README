# ✈️ ESP32 E-Paper Aircraft Tracker

A real-time aircraft tracking display using the OpenSky Network and a 4.2" e-paper display powered by an ESP32.

---

## 📦 Project Overview

This project uses an ESP32 microcontroller paired with a 4.2" e-paper display to show nearby aircraft data in real-time. It fetches live flight information using the OpenSky Network API and enhances it with aircraft metadata from PlaneSpotters. Key features include:

- 📡 **Live aircraft tracking** via OpenSky REST API  
- 🌐 **Captive portal for Wi-Fi and API configuration**  
- 🕒 **Time zone-aware clock rendering**  
- 🛩 **Aircraft metadata display** (model, callsign, country, bearing, distance)  
- 📟 **Flicker-free partial updates on e-paper display**  
- 🧠 **On-device caching** for up to 30 aircraft entries  

---

## 🧱 Hardware Requirements

| Component               | Notes                                 |
|------------------------|----------------------------------------|
| ESP32 Dev Board        | At least 4MB flash (any model)         |
| Waveshare 4.2" E-Paper | Use the `epd4in2_V2` model              |
| Pushbutton             | For entering Config Mode (GPIO 2)      |
| Power Supply           | USB or LiPo battery (as preferred)     |

---

## 🗂 File Structure

src/
├── main.cpp # Main application entry point
├── config.h / config.cpp # Captive portal and preference saving
├── display.h / display.cpp # All e-paper rendering logic
├── fetch.h / fetch.cpp # API interaction (OpenSky + PlaneSpotters)
├── cache.h / cache.cpp # Circular cache for aircraft data
├── OpenSkyAuthClient.h/.cpp # OAuth2 token management


---

## ⚙️ How It Works

### 🔧 1. Config Mode (On First Boot or Reset)

- Hold GPIO 2 LOW during boot to enter Config Mode.
- ESP32 starts a Wi-Fi AP named `ESP_Config`.
- Open a browser and visit `http://192.168.4.1`.
- Fill in:
  - Wi-Fi SSID & Password
  - OpenSky Client ID & Secret
  - Home Latitude, Longitude, Zoom level
  - Timezone offset (e.g., +05:30) and 24h format
- Settings are saved in non-volatile memory (Preferences).

### 🚀 2. Normal Mode

- ESP32 connects to Wi-Fi using saved credentials.
- Fetches a valid OAuth2 token from OpenSky.
- Every 15 seconds:
  - Fetches aircraft within a bounding box (based on home coordinates and zoom).
  - For each aircraft:
    - Fetches metadata from PlaneSpotters/OpenSky.
    - Caches model, callsign, bearing, distance.
- Updates e-paper display with aircraft info.

### 🖥 3. Display Modes

- **No Aircraft Nearby:**
  - Displays local time, date, and a message.
  - Uses partial update (no flicker).

- **Aircraft Nearby:**
  - Displays up to 5 aircraft.
  - Alternates entries using inverted color blocks.
  - Shows time and aircraft count in the header.

---

## 🧠 Key Features

### 🕒 Timezone-Aware Clock

- Timezone offset is user-configured (e.g., `+05:30` → `+330` minutes).
- Uses `strftime` for proper formatting after applying offset.

### 🧠 Caching System

- `AircraftCacheEntry[]` includes:
  - `icao24`, `model`, `callsign`, `distance`, `bearing`, `active`
- Ring buffer with 30 entries (cycled via `cacheIndex`).
- Sorted by distance using `sortAircraftCacheByDistance()`.

### 🖼 E-Paper Refresh Logic

- **Partial updates** for normal refreshes (e.g., aircraft or time change).
- **Full updates** only when switching between states (e.g., no aircraft → aircraft detected).

---

## 🔧 Setup (PlatformIO)

Your `platformio.ini` should look like:

```ini
[env:esp32dev]
platform = espressif32
board = esp32dev
framework = arduino
lib_deps =
  bblanchon/ArduinoJson@^6.21.3
  # Add e-paper display library here (e.g., GxEPD2 or Waveshare)
build_flags =
  -DCORE_DEBUG_LEVEL=3
