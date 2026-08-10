#include "solar_assistant_bridge.h"
#include "solar_assistant_mapping.h"
#include "secrets.h"

SolarAssistantBridge::SolarAssistantBridge(TuyaCloudClient& client) : _tuyaClient(client) {
    for (int i = 0; i < virtualMapSize; i++) {
        _cache[i].changed = false;
    }
}

void SolarAssistantBridge::handleMqttMessage(const String& topic, const String& payload) {
    for (int i = 0; i < virtualMapSize; i++) {
        if (topic == virtualDeviceMap[i].mqttTopic) {
            Serial.printf("[SA-Bridge] MQTT update for %s: %s\n", virtualDeviceMap[i].tuyaCode, payload.c_str());
            
            if (_cache[i].value != payload) {
                _cache[i].value = payload;
                _cache[i].code = virtualDeviceMap[i].tuyaCode;
                _cache[i].changed = true;
                _cache[i].lastUpdate = millis();
            }
            return;
        }
    }
}

void SolarAssistantBridge::processPendingCommands() {
    std::vector<TuyaCloudClient::TuyaDpValue> toReport;

    for (int i = 0; i < virtualMapSize; i++) {
        if (_cache[i].changed) {
            bool isStr = false;
            if (!(_cache[i].value.length() > 0 &&
               ((isdigit(_cache[i].value[0]) || _cache[i].value[0] == '-') &&
                 (_cache[i].value.indexOf('.') == -1 || _cache[i].value.indexOf('.') != -1)))) {
                isStr = true;
            }
            toReport.push_back({virtualDeviceMap[i].dpId, _cache[i].value, isStr});
        }
    }

    if (toReport.empty()) return;

    if (_tuyaClient.reportDPs(TUYA_VIRTUAL_ID, toReport)) {
        for (const auto& dp : toReport) {
            for (int i = 0; i < virtualMapSize; i++) {
                if (String(virtualDeviceMap[i].dpId) == dp.dpId) {
                    Serial.printf("[Tuya] Report OK: %s=%s\n", virtualDeviceMap[i].tuyaCode, dp.value.c_str());
                    _cache[i].changed = false;
                    break;
                }
            }
        }
            } else {
        Serial.println("[Tuya] Batch report failed");
            }
}

