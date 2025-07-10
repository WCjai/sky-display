// config.cpp
#include "config.h"
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
Preferences prefs;
WebServer server(80);
bool shouldReboot = false;

String WIFI_SSID, WIFI_PASSWORD;
String CLIENT_ID, CLIENT_SECRET;
float HOME_LAT;
float HOME_LON;
uint8_t ZOOM;
String TZ;
int TZ_minutes;
bool USE_24H = true;

// const char* form_html = R"rawliteral(
// <!DOCTYPE HTML>
// <html lang="en">
// <head>
//   <meta charset="UTF-8">
//   <meta name="viewport" content="width=device-width, initial-scale=1">
//   <title>Sky Display Configurator</title>
//   <style>
//     body {
//       margin: 0;
//       padding: 0;
//       font-family: Arial, sans-serif;
//       background: linear-gradient(to bottom, #87ceeb, #ffffff);
//     }
//     .header {
//       background-color: #004466;
//       color: #ffffff;
//       padding: 20px;
//       text-align: center;
//     }
//     .header .logo {
//       font-size: 1.5em;
//     }
//     .container {
//       max-width: 600px;
//       margin: 40px auto;
//       background-color: rgba(255,255,255,0.9);
//       padding: 20px;
//       border-radius: 8px;
//       box-shadow: 0 2px 8px rgba(0,0,0,0.2);
//     }
//     h2 {
//       text-align: center;
//       color: #004466;
//     }
//     .form-group {
//       margin-bottom: 15px;
//     }
//     .form-group label {
//       display: block;
//       margin-bottom: 5px;
//       font-weight: bold;
//       color: #004466;
//     }
//     .form-group input,
//     .form-group select {
//       width: 100%;
//       padding: 8px;
//       border: 1px solid #cccccc;
//       border-radius: 4px;
//       font-size: 1em;
//     }
//     .checkbox-group {
//       display: flex;
//       align-items: center;
//       margin-bottom: 15px;
//     }
//     .checkbox-group input {
//       margin-right: 10px;
//     }
//     .btn {
//       width: 100%;
//       padding: 12px;
//       background-color: #004466;
//       color: #ffffff;
//       border: none;
//       border-radius: 4px;
//       font-size: 1em;
//       cursor: pointer;
//     }
//     .btn:hover {
//       background-color: #003355;
//     }
//     .footer {
//       text-align: center;
//       margin-top: 20px;
//       font-size: 0.9em;
//       color: #666666;
//     }
//   </style>
// </head>
// <body>
//   <div class="header">
//     <div class="logo">Sky Display configurator</div>
//   </div>
//   <div class="container">
//     <h2>Settings</h2>
//     <form action="/save" method="POST">
//       <div class="form-group">
//         <label for="ssid">Wi-Fi SSID</label>
//         <input id="ssid"  name="ssid" type="text"    value="{ssid}">
//       </div>
//       <div class="form-group">
//         <label for="pass">Wi-Fi Password</label>
//         <input id="pass"  name="pass" type="password" value="{pass}">
//       </div>
//       <div class="form-group">
//         <label for="cid">Client ID</label>
//         <input id="cid"   name="cid"  type="text"     value="{cid}">
//       </div>
//       <div class="form-group">
//         <label for="csec">Client Secret</label>
//         <input id="csec"  name="csec" type="text"     value="{csec}">
//       </div>
//       <div class="form-group">
//         <label for="lat">Home Latitude</label>
//         <input id="lat"   name="lat"  type="text"     value="{lat}">
//       </div>
//       <div class="form-group">
//         <label for="lon">Home Longitude</label>
//         <input id="lon"   name="lon"  type="text"     value="{lon}">
//       </div>
//       <div class="form-group">
//         <label for="zoom">Map Zoom Level</label>
//         <input id="zoom"  name="zoom" type="number" min="8" max="15" value="{zoom}">
//         <small style="display:block; margin-top:8px; color:#004466;">
//           <strong>Approximate Radius covered by Zoom:</strong><br>
//           8 → 608 km, 9 → 417 km, 10 → 266 km, 11 → 186 km<br>
//           12 → 100 km, 13 → 62 km, 14 → 31 km, 15 → 15 km
          
//         </small>
//       </div>
//       <div class="checkbox-group">
//         <input id="use24h" name="use24h" type="checkbox" {use24h_checked}>
//         <label for="use24h">Use 24-hour time</label>
//       </div>
//       <div class="form-group">
//         <label for="tz">Timezone</label>
//         <select id="tz" name="tz">
//           {tz_options}
//         </select>
//       </div>
//       <button type="submit" class="btn">Save Settings</button>
//     </form>
//         <div class="footer">
//       Powered by
//       <a href="https://opensky-network.org" target="_blank">OpenSky Network</a>
//       &amp;
//       <a href="https://www.planespotters.net" target="_blank">Planespotters</a>
//     </div>
//   </div>
// </body>
// </html>
// )rawliteral";

const char* form_html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="UTF-8">
  <title>Sky Display Configurator</title>
  <style>
    body {
      font-family: "Segoe UI", Roboto, sans-serif;
      background: #e6f0f8;
      padding: 20px;
      color: #002b45;
    }
    h2 {
      text-align: center;
      margin-bottom: 20px;
      color: #003459;
      font-weight: 600;
      letter-spacing: 1px;
    }
    form {
      max-width: 640px;
      margin: auto;
      background: #ffffff;
      padding: 24px;
      border-radius: 10px;
      box-shadow: 0 3px 12px rgba(0,0,0,0.1);
      border-top: 6px solid #007BFF;
    }
    .section {
      margin-bottom: 25px;
    }
    .section-title {
      font-size: 18px;
      font-weight: bold;
      margin-bottom: 10px;
      border-bottom: 1px solid #ccc;
      padding-bottom: 5px;
      color: #004172;
    }
    .row {
      display: grid;
      grid-template-columns: 1fr 1fr;
      gap: 20px;
      margin-bottom: 10px;
    }
    label {
      font-weight: 600;
      display: block;
      margin-bottom: 5px;
      color: #003459;
    }
    input[type="text"],
    input[type="password"],
    input[type="number"],
    select {
      width: 90%;
      padding: 7px 10px;
      border: 1px solid #a0b9d6;
      border-radius: 5px;
      font-size: 14px;
      background: #f8fbff;
    }
    .zoom-row {
      display: flex;
      align-items: center;
      gap: 10px;
      margin-top: 10px;
    }
    .info-button {
      background-color: #007BFF;
      color: white;
      border: none;
      border-radius: 50%;
      width: 22px;
      height: 22px;
      cursor: pointer;
      font-weight: bold;
      text-align: center;
      line-height: 20px;
    }
    .info-box {
      display: none;
      font-size: 13px;
      margin-top: 8px;
      background: #eef6ff;
      padding: 10px;
      border-radius: 5px;
      border-left: 3px solid #007BFF;
    }
    .toggle-visible {
      display: block !important;
    }
    .time-row {
      display: flex;
      align-items: center;
      gap: 20px;
      margin-top: 10px;
    }
    .checkbox-row {
      display: flex;
      align-items: center;
      gap: 8px;
    }
    .checkbox-row input[type="checkbox"] {
      margin: 0;
      transform: scale(1.2);
    }
    .submit-btn {
      display: block;
      width: 100%;
      padding: 12px;
      font-size: 16px;
      background: #007BFF;
      color: white;
      border: none;
      border-radius: 6px;
      cursor: pointer;
      letter-spacing: 0.5px;
    }
    .submit-btn:hover {
      background: #005ecb;
    }
  </style>
  <script>
    function toggleInfoBox() {
      var box = document.getElementById('zoom-info');
      box.classList.toggle('toggle-visible');
    }
  </script>
</head>
<body>
  <h2>🛫 Sky Display Configurator</h2>
  <form action="/save" method="POST">

    <div class="section">
      <div class="section-title">WiFi Credentials</div>
      <div class="row">
        <div>
          <label for="ssid">SSID</label>
          <input type="text" name="ssid" id="ssid" value="{ssid}">
        </div>
        <div>
          <label for="pass">Password</label>
          <input type="text" name="pass" id="pass" value="{pass}">
        </div>
      </div>
    </div>

    <div class="section">
      <div class="section-title">OpenSky Network Credentials</div>
      <div class="row">
        <div>
          <label for="cid">Client ID</label>
          <input type="text" name="cid" id="cid" value="{cid}">
        </div>
        <div>
          <label for="csec">Client Secret</label>
          <input type="text" name="csec" id="csec" value="{csec}">
        </div>
      </div>
    </div>

    <div class="section">
      <div class="section-title">Sky Location</div>
      <div class="row">
        <div>
          <label for="lat">Latitude</label>
          <input type="text" name="lat" id="lat" value="{lat}">
        </div>
        <div>
          <label for="lon">Longitude</label>
          <input type="text" name="lon" id="lon" value="{lon}">
        </div>
      </div>
      <div class="zoom-row">
        <div style="flex:1;">
          <label for="zoom">Map Zoom</label>
          <input type="number" name="zoom" id="zoom" value="{zoom}" min="8" max="15">
          <button type="button" class="info-button" onclick="toggleInfoBox()">i</button>
        </div>
        
      </div>
      <div id="zoom-info" class="info-box">
        <strong>Approximate Radius by Zoom:</strong><br>
        Zoom 8 → 608 km<br>
        Zoom 9 → 417 km<br>
        Zoom 10 → 266 km<br>
        Zoom 11 → 186 km<br>
        Zoom 12 → 100 km<br>
        Zoom 13 → 62 km<br>
        Zoom 14 → 31 km<br>
        Zoom 15 → 15 km
      </div>
    </div>

    <div class="section">
      <div class="section-title">Display time</div>
      <div class="time-row">
        <div style="flex:1;">
          <label for="tz">Time Zone (minutes offset)</label>
          <select id="tz" name="tz">{tz_options}</select>
        </div>
        <div class="checkbox-row" style="flex:1; margin-top: 26px;">
          <input type="checkbox" name="use24h" id="use24h" {use24_checked}>
          <label for="use24h">Use 24-hour format</label>
        </div>
      </div>
    </div>

    <button class="submit-btn" type="submit">Save Settings</button>
  </form>
</body>
</html>
)rawliteral";




static const char* TZ_OFFSETS[] = {
  "-12:00","-11:00","-10:00","-09:30","-09:00","-08:00","-07:00","-06:00",
  "-05:00","-04:00","-03:30","-03:00","-02:00","-01:00","+00:00","+01:00",
  "+02:00","+03:00","+03:30","+04:00","+04:30","+05:00","+05:30","+05:45",
  "+06:00","+06:30","+07:00","+08:00","+08:45","+09:00","+09:30","+10:00",
  "+10:30","+11:00","+12:00","+12:45","+13:00","+14:00"
};
const int TZ_COUNT = sizeof(TZ_OFFSETS) / sizeof(TZ_OFFSETS[0]);

String fillForm() {
  String s = form_html;
  s.replace("{ssid}",  WIFI_SSID);
  s.replace("{pass}",  WIFI_PASSWORD);
  s.replace("{cid}",   CLIENT_ID);
  s.replace("{csec}",  CLIENT_SECRET);
  s.replace("{lat}",   String(HOME_LAT,5));
  s.replace("{lon}",   String(HOME_LON,5));
  s.replace("{zoom}",  String(ZOOM));

  String tzOpts;
  for (int i = 0; i < TZ_COUNT; ++i) {
    const char* off = TZ_OFFSETS[i];
    bool sel = TZ.equals(off);
    tzOpts += "<option value=\"";
    tzOpts += off;
    tzOpts += "\"";
    if (sel) tzOpts += " selected";
    tzOpts += ">UTC";
    tzOpts += off;
    tzOpts += "</option>\n";
  }
  s.replace("{tz_options}", tzOpts);
  s.replace("{use24h_checked}", USE_24H ? "checked" : "");
  return s;
}

void handleRoot() {
  server.send(200, "text/html", fillForm());
}

void handleSave() {
  WIFI_SSID       = server.arg("ssid");
  WIFI_PASSWORD   = server.arg("pass");
  CLIENT_ID       = server.arg("cid");
  CLIENT_SECRET   = server.arg("csec");
  HOME_LAT        = server.arg("lat").toFloat();
  HOME_LON        = server.arg("lon").toFloat();
  ZOOM            = (uint8_t)server.arg("zoom").toInt();
  USE_24H         = server.hasArg("use24h");
  TZ              = server.arg("tz");

  int sign = (TZ.charAt(0)=='-') ? -1 : 1;
  int hh   = TZ.substring(1,3).toInt();
  int mm   = TZ.substring(4,6).toInt();
  TZ_minutes = sign * (hh*60 + mm);

  prefs.begin("cfg", false);
  prefs.putString ("ssid", WIFI_SSID);
  prefs.putString ("pass", WIFI_PASSWORD);
  prefs.putString ("cid",  CLIENT_ID);
  prefs.putString ("csec", CLIENT_SECRET);
  prefs.putFloat  ("lat",  HOME_LAT);
  prefs.putFloat  ("lon",  HOME_LON);
  prefs.putUInt   ("zoom", ZOOM);
  prefs.putString ("tz",   TZ);
  prefs.putInt    ("tz_mins", TZ_minutes);
  prefs.putBool   ("use24h", USE_24H);
  prefs.end();

  server.send(200, "text/html", R"rawliteral(
  <!DOCTYPE html>
  <html>
  <head>
    <meta charset="UTF-8">
    <title>Settings Saved</title>
    <style>
      body {
        font-family: "Segoe UI", Roboto, sans-serif;
        background: #e6f0f8;
        display: flex;
        justify-content: center;
        align-items: center;
        height: 100vh;
        margin: 0;
        color: #003459;
      }
      .card {
        background: #ffffff;
        padding: 30px 40px;
        border-radius: 10px;
        box-shadow: 0 3px 10px rgba(0,0,0,0.1);
        text-align: center;
        border-top: 6px solid #007BFF;
      }
      h2 {
        font-size: 22px;
        margin-bottom: 15px;
      }
      p {
        font-size: 14px;
        color: #555;
      }
    </style>
    <script>
      setTimeout(() => location.reload(), 2000);
    </script>
  </head>
  <body>
    <div class="card">
      <h2>✅ Settings Saved</h2>
      <p>Rebooting...</p>
    </div>
  </body>
  </html>
  )rawliteral");


  shouldReboot = true;
}

void startConfigMode() {
  prefs.begin("cfg", true);
  WIFI_SSID     = prefs.getString("ssid", "");
  WIFI_PASSWORD = prefs.getString("pass", "");
  CLIENT_ID     = prefs.getString("cid", "");
  CLIENT_SECRET = prefs.getString("csec", "");
  HOME_LAT      = prefs.getFloat("lat", 0.0f);
  HOME_LON      = prefs.getFloat("lon", 0.0f);
  ZOOM          = prefs.getUInt("zoom", 0);
  TZ            = prefs.getString("tz", "+05:30");
  TZ_minutes    = prefs.getInt("tz_mins", 330);
  USE_24H       = prefs.getBool("use24h", true);
  prefs.end();

  WiFi.mode(WIFI_AP);
  WiFi.softAP("WC_Sky_display");
  Serial.print("AP IP: "); Serial.println(WiFi.softAPIP());

  server.on("/", HTTP_GET, handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.begin();

  while (!shouldReboot) {
    server.handleClient();
    delay(10);
  }

  delay(500);
  ESP.restart();
}

void loadConfig() {
  prefs.begin("cfg", true);
  WIFI_SSID     = prefs.getString("ssid", "");
  WIFI_PASSWORD = prefs.getString("pass", "");
  CLIENT_ID     = prefs.getString("cid", "");
  CLIENT_SECRET = prefs.getString("csec", "");
  HOME_LAT      = prefs.getFloat("lat", 0.0f);
  HOME_LON      = prefs.getFloat("lon", 0.0f);
  ZOOM          = prefs.getUInt("zoom", 0);
  TZ            = prefs.getString("tz", "+05:30");
  TZ_minutes    = prefs.getInt("tz_mins", 330);
  USE_24H       = prefs.getBool("use24h", true);
  prefs.end();
}
