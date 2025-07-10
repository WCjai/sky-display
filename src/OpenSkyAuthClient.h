#ifndef OPENSKY_AUTH_CLIENT_H
#define OPENSKY_AUTH_CLIENT_H

#include <Arduino.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

class OpenSkyAuthClient {
public:
    OpenSkyAuthClient(const String& clientId, const String& clientSecret);

    bool ensureValidToken();               // Checks and refreshes token if needed
    String getAccessToken();               // Returns current access token
    bool isTokenValid();                   // Token still valid?

private:
    String _clientId;
    String _clientSecret;
    String _accessToken;
    unsigned long _tokenExpiry;            // millis-based

    bool fetchNewToken();
};

#endif
