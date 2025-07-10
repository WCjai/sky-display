#include "OpenSkyAuthClient.h"

OpenSkyAuthClient::OpenSkyAuthClient(const String& clientId, const String& clientSecret)
    : _clientId(clientId), _clientSecret(clientSecret), _accessToken(""), _tokenExpiry(0) {}

bool OpenSkyAuthClient::fetchNewToken() {
    HTTPClient http;
    http.begin("https://auth.opensky-network.org/auth/realms/opensky-network/protocol/openid-connect/token");
    http.addHeader("Content-Type", "application/x-www-form-urlencoded");

    String body = "grant_type=client_credentials"
                  "&client_id=" + _clientId +
                  "&client_secret=" + _clientSecret;

    int httpCode = http.POST(body);

    if (httpCode == 200) {
        String payload = http.getString();
        DynamicJsonDocument doc(2048);
        DeserializationError err = deserializeJson(doc, payload);
        if (err) {
            Serial.println("❌ Token JSON parse error");
            http.end();
            return false;
        }

        _accessToken = doc["access_token"].as<String>();
        int expiresIn = doc["expires_in"] | 300;
        _tokenExpiry = millis() + (expiresIn - 10) * 1000;

        Serial.println("✅ OpenSky token fetched.");
        http.end();
        return true;
    } else {
        Serial.printf("❌ Token fetch HTTP error: %d\n", httpCode);
        http.end();
        return false;
    }
}

bool OpenSkyAuthClient::ensureValidToken() {
    if (_accessToken == "" || millis() > _tokenExpiry) {
        Serial.println("[OpenSkyAuthClient] Token expired or empty. Fetching new...");
        return fetchNewToken();
    }
    return true;
}

bool OpenSkyAuthClient::isTokenValid() {
    return !_accessToken.isEmpty() && millis() < _tokenExpiry;
}

String OpenSkyAuthClient::getAccessToken() {
    return _accessToken;
}
