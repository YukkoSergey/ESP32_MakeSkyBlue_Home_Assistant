#include "solar_assistant_bridge.h"
#include "app_log.h"
#include "solar_assistant_mapping.h"
#include "secrets.h"

SolarAssistantBridge::SolarAssistantBridge(TuyaCloudClient& client) : _tuyaClient(client) {
    _mutex = xSemaphoreCreateMutex();
    for (int i = 0; i < virtualMapSize; i++) {
        _cache[i].changed = false;
    }
}

void SolarAssistantBridge::handleMqttMessage(const String& topic, const String& payload) {
    for (int i = 0; i < virtualMapSize; i++) {
        if (topic == virtualDeviceMap[i].mqttTopic) {
            xSemaphoreTake(_mutex, portMAX_DELAY);
            if (_cache[i].value != payload) {
                // Only log changes — SA republishes unchanged values every few
                // seconds and would flood the web log buffer.
                Log.printf("[SA-Bridge] MQTT update for %s: %s\n", virtualDeviceMap[i].tuyaCode, payload.c_str());
                _cache[i].value = payload;
                _cache[i].code = virtualDeviceMap[i].tuyaCode;
                _cache[i].changed = true;
                _cache[i].lastUpdate = millis();
            }
            xSemaphoreGive(_mutex);
            return;
        }
    }
}

void SolarAssistantBridge::processPendingCommands() {
    std::vector<TuyaCloudClient::TuyaDpValue> toReport;

    xSemaphoreTake(_mutex, portMAX_DELAY);
    for (int i = 0; i < virtualMapSize; i++) {
        if (_cache[i].changed) {
            // Treat as number if the whole payload looks numeric (optional sign,
            // digits, optional single decimal point). Anything else -> string DP.
            const String& v = _cache[i].value;
            bool numeric = v.length() > 0;
            bool seenDot = false;
            for (unsigned k = 0; k < v.length() && numeric; k++) {
                char c = v[k];
                if (k == 0 && (c == '-' || c == '+')) continue;
                if (c == '.') { if (seenDot) { numeric = false; break; } seenDot = true; continue; }
                if (!isdigit((unsigned char)c)) numeric = false;
            }
            toReport.push_back({virtualDeviceMap[i].dpId, v, /*isString=*/!numeric});
        }
    }

    xSemaphoreGive(_mutex);

    if (toReport.empty()) return;

    if (_tuyaClient.reportDPs(TUYA_VIRTUAL_ID, toReport)) {
        for (const auto& dp : toReport) {
            for (int i = 0; i < virtualMapSize; i++) {
                if (String(virtualDeviceMap[i].dpId) == dp.dpId) {
                    Log.printf("[Tuya] Report OK: %s=%s\n", virtualDeviceMap[i].tuyaCode, dp.value.c_str());
                    // A newer value may have arrived while we were reporting —
                    // keep it pending in that case.
                    xSemaphoreTake(_mutex, portMAX_DELAY);
                    if (_cache[i].value == dp.value) _cache[i].changed = false;
                    xSemaphoreGive(_mutex);
                    break;
                }
            }
        }
            } else {
        Log.println("[Tuya] Batch report failed");
            }
}

