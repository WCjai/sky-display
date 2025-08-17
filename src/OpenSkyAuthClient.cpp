#include "OpenSkyAuthClient.h"

OpenSkyAuthClient::OpenSkyAuthClient(const String& clientId, const String& clientSecret)
: _clientId(clientId), _clientSecret(clientSecret), _accessToken(""), _tokenExpiry(0), _mtx(nullptr) {
    _mtx = xSemaphoreCreateMutex();
}

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
        JsonDocument doc;
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
    if (_mtx) xSemaphoreTake(_mtx, portMAX_DELAY);
    bool need = (_accessToken == "" || millis() > _tokenExpiry);
    if (!need) {
        if (_mtx) xSemaphoreGive(_mtx);
        return true;
    }
    // Release lock while doing HTTP to avoid blocking other callers too long
    if (_mtx) xSemaphoreGive(_mtx);

    bool ok = fetchNewToken();

    if (_mtx) xSemaphoreTake(_mtx, portMAX_DELAY);
    // nothing else to do; fetchNewToken already set members
    if (_mtx) xSemaphoreGive(_mtx);
    return ok;
}

bool OpenSkyAuthClient::isTokenValid() {
    if (_mtx) xSemaphoreTake(_mtx, portMAX_DELAY);
    bool ok = !_accessToken.isEmpty() && millis() < _tokenExpiry;
    if (_mtx) xSemaphoreGive(_mtx);
    return ok;
}

String OpenSkyAuthClient::getAccessToken() {
    if (_mtx) xSemaphoreTake(_mtx, portMAX_DELAY);
    String tok = _accessToken;
    if (_mtx) xSemaphoreGive(_mtx);
    return tok;
}