#include "solar_manager_protocol.h"
#include "sm_config.h"
#include <ArduinoJson.h>
#include <WiFi.h>
#include <PubSubClient.h>

SolarManagerProtocol::Config SolarManagerProtocol::_config;
uint32_t SolarManagerProtocol::_lastNotifyTime = 0;
uint32_t SolarManagerProtocol::_lastDiagTime = 0;

void SolarManagerProtocol::init(const char* serial, const char* firmware) {
    strncpy(_config.serial, serial, sizeof(_config.serial) - 1);
    strncpy(_config.firmwareVersion, firmware, sizeof(_config.firmwareVersion) - 1);

    // Default verified segments
    _config.segments[0] = {1, 4, 1, 12}; // FC4 1-12
    _config.segments[1] = {1, 4, 26, 1};  // FC4 26-26
    _config.segments[2] = {1, 3, 1, 10}; // FC3 1-10
    _config.segmentCount = 3;

    Serial.printf("[SM] Protocol initialized for serial: %s\n", _config.serial);
}

void SolarManagerProtocol::publishOnline(PubSubClient& mqtt) {
    String topic = String(_config.serial) + "/online";
    mqtt.publish(topic.c_str(), _config.firmwareVersion); 
    Serial.printf("[SM] Published online: %s\n", _config.firmwareVersion);
}

uint16_t SolarManagerProtocol::mapChargeMode(const String& mode) {
    Serial.printf("[SM] Mapping charge mode: %s\n", mode.c_str());
    return 0; 
}

uint16_t SolarManagerProtocol::mapBatteryType(const String& type) {
    if (type.equalsIgnoreCase("lithium")) return 1;
    return 0;
}

void SolarManagerProtocol::sendNotifyFrame(PubSubClient& mqtt, uint8_t slave, uint8_t fn, uint16_t addr, uint16_t len, const MpptState& state) {
    uint8_t buffer[128];
    if (6 + (len * 2) > sizeof(buffer)) return;

    buffer[0] = slave;
    buffer[1] = fn;
    buffer[2] = (addr >> 8) & 0xFF;
    buffer[3] = addr & 0xFF;
    buffer[4] = (len >> 8) & 0xFF;
    buffer[5] = len & 0xFF;

    for (int i = 0; i < len; i++) {
        uint16_t regAddr = addr + i;
        uint16_t value = 0;

        if (fn == 4) {
            switch (regAddr) {
                case 1: value = state.fault; break;
                case 5: value = (uint16_t)round(state.batteryVoltage * 10); break;
                case 6: value = (uint16_t)round(state.batteryCurrent * 10); break;
                case 7: value = (uint16_t)round(state.pvVoltage * 10); break;
                case 8: value = (uint16_t)round(state.power); break;
                case 9: value = (uint16_t)round(state.temperature * 10); break;
                case 10: value = (uint16_t)round(state.totalEnergy); break;
                case 11: value = 0; break;
                case 12: value = mapChargeMode(state.chargeMode); break;
                case 26: value = 0; break;
                default: value = 0; break;
            }
        } else if (fn == 3) {
            switch (regAddr) {
                case 1: value = (uint16_t)round(state.equalizationVoltage * 10); break;
                case 2: value = (uint16_t)round(state.floatVoltage * 10); break;
                case 3: value = (uint16_t)state.outputTimer; break;
                case 4: value = (uint16_t)(state.chargeCurrent * 10); break;
                case 5: value = (uint16_t)round(state.lowVoltage * 10); break;
                case 6: value = (uint16_t)round(state.recoveryVoltage * 10); break;
                case 7: value = 1; break;
                case 8: value = mapBatteryType(state.batteryType); break;
                case 9: value = (uint16_t)state.batteryCells; break;
                case 10: value = 0; break;
                default: value = 0; break;
            }
        }
        
        buffer[6 + i*2] = (value >> 8) & 0xFF;
        buffer[7 + i*2] = value & 0xFF;
    }

    String topic = String(_config.serial) + "/notify";
    mqtt.publish(topic.c_str(), buffer, 6 + (len * 2));
}

void SolarManagerProtocol::sendDiagnostics(PubSubClient& mqtt) {
    StaticJsonDocument<128> doc;
    doc["ssid"] = WiFi.SSID();
    doc["rssi"] = WiFi.RSSI();
    doc["led"] = _config.ledOn ? "on" : "off";
    
    String payload;
    serializeJson(doc, payload);
    
    String topic = String(_config.serial) + "/diagnostics";
    mqtt.publish(topic.c_str(), payload.c_str());
}

void SolarManagerProtocol::processPeriodicTasks(PubSubClient& mqtt, const MpptState& state) {
    uint32_t now = millis();
    
    if (now - _lastNotifyTime >= 5000) {
        _lastNotifyTime = now;
        for (int i = 0; i < _config.segmentCount; i++) {
            sendNotifyFrame(mqtt, _config.segments[i].slaveId, _config.segments[i].function, 
                             _config.segments[i].startAddress, _config.segments[i].length, state);
        }
    }
    
    if (now - _lastDiagTime >= 30000) {
        _lastDiagTime = now;
        sendDiagnostics(mqtt);
    }
}

void SolarManagerProtocol::handleMqttMessage(const String& topic, const String& payload, PubSubClient& mqtt) {
    String serial = _config.serial;
    if (topic == serial + "/config") {
        StaticJsonDocument<256> doc;
        if (deserializeJson(doc, payload) == DeserializationError::Ok) {
            JsonArray segments = doc["segments"];
            _config.segmentCount = 0;
            for (JsonObject seg : segments) {
                if (_config.segmentCount < 5) {
                    _config.segments[_config.segmentCount++] = {
                        seg["slave_id"], seg["read_command"], seg["start_address"], seg["length"]
                    };
                }
            }
            Serial.printf("[SM] Config received. Segments: %d\n", _config.segmentCount);
        }
    } else if (topic.startsWith(serial + "/control/")) {
        if (topic == serial + "/control/restart") {
            Serial.println("[SM] RESTART DISABLED IN MVP");
        } else if (topic == serial + "/control/led") {
            _config.ledOn = (payload == "on");
        } else {
            Serial.printf("[SM] control/cmd: %s. WRITE DISABLED IN MVP\n", payload.c_str());
        }
    }
}

