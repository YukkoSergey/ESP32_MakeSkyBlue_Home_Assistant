#ifndef SOLAR_MANAGER_PROTOCOL_H
#define SOLAR_MANAGER_PROTOCOL_H

#include <Arduino.h>
#include "sm_config.h"
#include <PubSubClient.h>
#include "mppt_bridge.h"

class SolarManagerProtocol {
public:
    struct Config {
        char serial[32] = "MSB_MPPT_1";
        char firmwareVersion[16] = "1.0.0";
        SmSegment segments[5];
        uint8_t segmentCount = 0;
        bool ledOn = true;
    };

    static void init(const char* serial, const char* firmware);
    static void handleMqttMessage(const String& topic, const String& payload, PubSubClient& mqtt);
    static void processPeriodicTasks(PubSubClient& mqtt, const MpptState& state);
    static void publishOnline(PubSubClient& mqtt);

private:
    static Config _config;
    static uint32_t _lastNotifyTime;
    static uint32_t _lastDiagTime;

    static void sendNotifyFrame(PubSubClient& mqtt, uint8_t slave, uint8_t fn, uint16_t addr, uint16_t len, const MpptState& state);
    static void sendDiagnostics(PubSubClient& mqtt);
    static uint16_t mapChargeMode(const String& mode);
    static uint16_t mapBatteryType(const String& type);
};

#endif


