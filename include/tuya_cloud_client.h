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

    String makeNonce();
    String hmacHex(const String &in);
    String sha256Hex(const String &in);

    // ДОДАЙТЕ ЦЕЙ РЯДОК НИЖЧЕ:
    bool sendRequest(const String& deviceId, const String& dpCode, const String& valueStr, bool isStringValue);

    bool sendCommand(const String& deviceId, const String& dpCode, float value);
    bool sendCommandString(const String& deviceId, const String& dpCode, const String& value);
};

#endif