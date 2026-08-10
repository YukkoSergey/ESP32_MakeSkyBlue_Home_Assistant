#include "tuya_cloud_client.h"
#include <mbedtls/md.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>

TuyaCloudClient::TuyaCloudClient(const char* baseUrl, const char* accessId, const char* accessSecret)
    : _baseUrl(baseUrl), _accessId(accessId), _accessSecret(accessSecret), _accessToken("") {}

String TuyaCloudClient::makeNonce() {
    uint32_t r;
    esp_fill_random(&r, sizeof(r));
    char b[9];
    sprintf(b, "%08X", r);
    return String(b);
}

String TuyaCloudClient::hmacHex(const String &in) {
    mbedtls_md_context_t ctx;
    auto info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    mbedtls_md_init(&ctx);
    mbedtls_md_setup(&ctx, info, 1);
    
    // Використовуємо .c_str() для коректного приведення String до const unsigned char*
    mbedtls_md_hmac_starts(&ctx, (const unsigned char*)_accessSecret.c_str(), _accessSecret.length());
    mbedtls_md_hmac_update(&ctx, (const unsigned char*)in.c_str(), in.length());

    uint8_t mac[32];
    mbedtls_md_hmac_finish(&ctx, mac);
    mbedtls_md_free(&ctx);

    char out[65];
    for (int i = 0; i < 32; i++) sprintf(out + i*2, "%02X", mac[i]);
    out[64] = 0;
    return String(out);
}

String TuyaCloudClient::sha256Hex(const String &in) {
    uint8_t hash[32];
    // Використовуємо .c_str() для коректного приведення String до const unsigned char*
    mbedtls_md(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), (const unsigned char*)in.c_str(), in.length(), hash);

    char out[65];
    for (int i = 0; i < 32; i++) sprintf(out + i*2, "%02x", hash[i]);
    out[64] = 0;
    return String(out);
}

bool TuyaCloudClient::updateAccessToken() {
    uint64_t nowMs = (uint64_t)time(nullptr) * 1000ULL;
    String ts = String(nowMs);
    String nonce = makeNonce();
    String emptySha = sha256Hex("");

    String path = "/v1.0/token?grant_type=1";
    String stringToSign = String("GET\n") + emptySha + "\n\n" + path;
    String toSign = String(_accessId) + ts + nonce + stringToSign;
    String sign = hmacHex(toSign);

    WiFiClientSecure secureClient;
    secureClient.setInsecure();

    HTTPClient http;
    String url = String(_baseUrl) + path;

    if (!http.begin(secureClient, url)) return false;
    
    http.addHeader("client_id", _accessId);
    http.addHeader("t", ts);
    http.addHeader("nonce", nonce);
    http.addHeader("sign_method", "HMAC-SHA256");
    http.addHeader("sign", sign);

    int code = http.GET();
    if (code != 200) {
        Serial.printf("[Tuya] Token fetch failed: %d\n", code);
        http.end();
        return false;
    }

    String resp = http.getString();
    StaticJsonDocument<512> doc;
    deserializeJson(doc, resp);
    _accessToken = doc["result"]["access_token"].as<String>();

    http.end();
    return true;
}

bool TuyaCloudClient::reportDeviceDPs(const String& deviceId, const String& dpCode, int value) {
    return sendCommand(deviceId, dpCode, (float)value);
}

bool TuyaCloudClient::reportDeviceDPs(const String& deviceId, const String& dpCode, float value) {
    return sendCommand(deviceId, dpCode, value);
}

bool TuyaCloudClient::reportDeviceDPs(const String& deviceId, const String& dpCode, const String& value) {
    return sendCommandString(deviceId, dpCode, value);
}

bool TuyaCloudClient::sendRequest(const String& deviceId, const String& dpCode, const String& valueStr, bool isStringValue) {
    if (_accessToken == "") {
        if (!updateAccessToken()) {
            Serial.println("[Tuya] Failed to get access token before request");
            return false;
        }
    }

    uint64_t nowMs = (uint64_t)time(nullptr) * 1000ULL;
    String ts = String(nowMs);
    String nonce = makeNonce();

    // NEW PAYLOAD FORMAT for /datapoint/report
    StaticJsonDocument<256> doc;
    JsonObject data = doc.createNestedObject("data");

    // We need the DP ID here. Since sendRequest currently takes dpCode,
    // we assume dpCode passed here is actually the DP ID for this endpoint.
    JsonObject dpObj = data.createNestedObject(dpCode);
    if (isStringValue) {
        dpObj["value"] = valueStr;
    } else {
        float fval = valueStr.toFloat();
        if (valueStr.indexOf('.') != -1) {
            dpObj["value"] = fval;
        } else {
            dpObj["value"] = valueStr.toInt();
        }
    }
    dpObj["time"] = nowMs;

    String body;
    serializeJson(doc, body);

    String bodySha = sha256Hex(body);
    // CHANGED ENDPOINT
    String path = "/v1.0/cloud/thing/devices/" + deviceId + "/datapoint/report";
    String canonical = String("POST\n") + bodySha + "\n\n" + path;
    String toSign = String(_accessId) + _accessToken + ts + nonce + canonical;
    String sign = hmacHex(toSign);

    WiFiClientSecure secureClient;
    secureClient.setInsecure();

    HTTPClient http;
    String url = String(_baseUrl) + path;

    if (!http.begin(secureClient, url)) {
        Serial.println("[Tuya] HTTP connection failed");
        return false;
    }

    http.addHeader("client_id", _accessId);
    http.addHeader("access_token", _accessToken);
    http.addHeader("t", ts);
    http.addHeader("nonce", nonce);
    http.addHeader("sign_method", "HMAC-SHA256");
    http.addHeader("sign", sign);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Content-Sha256", bodySha);

    Serial.printf("[Tuya-DEBUG] POST to: %s\n", url.c_str());
    Serial.printf("[Tuya-DEBUG] DeviceID: %s\n", deviceId.c_str());
    Serial.printf("[Tuya-DEBUG] Body: %s\n", body.c_str());

    int httpResponseCode = http.POST(body);
    String resp = http.getString();

    Serial.printf("[Tuya-DEBUG] HTTP Status: %d\n", httpResponseCode);
    Serial.printf("[Tuya-DEBUG] Response: %s\n", resp.c_str());

    http.end();

    if (httpResponseCode != 200) {
        Serial.printf("[Tuya] HTTP Error: %d | Response: %s\n", httpResponseCode, resp.c_str());
        return false;
    }

    StaticJsonDocument<512> resDoc;
    deserializeJson(resDoc, resp);
    bool success = resDoc["success"];
    
    if (!success) {
        Serial.printf("[Tuya] API Error: success=false | Response: %s\n", resp.c_str());
    }

    return success;
}

bool TuyaCloudClient::sendCommand(const String& deviceId, const String& dpCode, float value) {
    return sendRequest(deviceId, dpCode, String(value), false);
}

bool TuyaCloudClient::sendCommandString(const String& deviceId, const String& dpCode, const String& value) {
    return sendRequest(deviceId, dpCode, value, true);
}

bool TuyaCloudClient::reportDPs(const String& deviceId, const std::vector<TuyaDpValue>& dpValues) {
    if (dpValues.empty()) return true;

    // Since reportDeviceDPs / sendRequest handles single DPs,
    // and we don't have a batch API implementation yet,
    // we iterate through the values.
    bool allSuccess = true;
    for (const auto& dp : dpValues) {
        if (dp.isString) {
            if (!reportDeviceDPs(deviceId, dp.dpId, dp.value)) allSuccess = false;
        } else {
            // Try to determine if it's float or int for reportDeviceDPs overload
            if (dp.value.indexOf('.') != -1) {
                if (!reportDeviceDPs(deviceId, dp.dpId, dp.value.toFloat())) allSuccess = false;
            } else {
                if (!reportDeviceDPs(deviceId, dp.dpId, (int)dp.value.toInt())) allSuccess = false;
            }
        }
    }
    return allSuccess;
}

String TuyaCloudClient::getDeviceProperties(const String& deviceId) {
    if (_accessToken == "") {
        if (!updateAccessToken()) return "";
    }

    uint64_t nowMs = (uint64_t)time(nullptr) * 1000ULL;
    String ts = String(nowMs);
    String nonce = makeNonce();

    // CHANGED PATH for properties
    String path = "/v1.0/cloud/thing/devices/" + deviceId + "/properties";
    String bodySha = sha256Hex("");

    String canonical = String("GET\n") + bodySha + "\n\n" + path;
    String toSign = String(_accessId) + _accessToken + ts + nonce + canonical;
    String sign = hmacHex(toSign);

    WiFiClientSecure secureClient;
    secureClient.setInsecure();

    HTTPClient http;
    String url = String(_baseUrl) + path;

    if (!http.begin(secureClient, url)) return "";

    http.addHeader("client_id", _accessId);
    http.addHeader("access_token", _accessToken);
    http.addHeader("t", ts);
    http.addHeader("nonce", nonce);
    http.addHeader("sign_method", "HMAC-SHA256");
    http.addHeader("sign", sign);

    int httpResponseCode = http.GET();
    String resp = http.getString();
    http.end();

    if (httpResponseCode != 200) return "";

    return resp;
}

