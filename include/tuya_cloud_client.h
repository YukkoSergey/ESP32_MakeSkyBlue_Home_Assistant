#ifndef TUYA_CLOUD_CLIENT_H
#define TUYA_CLOUD_CLIENT_H

#include <Arduino.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <vector>

class TuyaCloudClient {
public:
    struct TuyaDpValue {
        String dpId;
        String value;
        bool isString;
    };

    TuyaCloudClient(const char* baseUrl, const char* accessId, const char* accessSecret);

    // Returns true when _accessToken is present and not expired. Fetches or
    // refreshes on demand. Cheap when the token is still fresh — no HTTP.
    bool ensureToken();

    // Backward compatibility with existing callers.
    bool updateAccessToken();

    bool reportDeviceDPs(const String& deviceId, const String& dpId, int value);
    bool reportDeviceDPs(const String& deviceId, const String& dpId, float value);
    bool reportDeviceDPs(const String& deviceId, const String& dpId, const String& value);

    // New method for batch reporting
    bool reportDPs(const String& deviceId, const std::vector<TuyaDpValue>& dpValues);

    String getDeviceProperties(const String& deviceId);
private:
    String _baseUrl;
    String _accessId;
    String _accessSecret;
    String _accessToken;
    // Absolute expiry deadline in ms since unix epoch. 0 = no token yet.
    // Refresh is due when now + kTokenSafetyMarginMs >= _tokenExpiresAtMs.
    uint64_t _tokenExpiresAtMs = 0;
    static constexpr uint64_t kTokenSafetyMarginMs = 60ULL * 1000ULL; // 60s

    // Attempt a fresh /token grant. Called by ensureToken(). Not for direct
    // use by callers — they should invoke ensureToken() instead.
    bool fetchAccessToken();

    String makeNonce();
    String hmacHex(const String &in);
    String sha256Hex(const String &in);

    // ДОДАЙТЕ ЦЕЙ РЯДОК НИЖЧЕ:
    bool sendRequest(const String& deviceId, const String& dpCode, const String& valueStr, bool isStringValue);

    bool sendCommand(const String& deviceId, const String& dpCode, float value);
    bool sendCommandString(const String& deviceId, const String& dpCode, const String& value);
};

#endif