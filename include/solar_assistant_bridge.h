#ifndef SOLAR_ASSISTANT_BRIDGE_H
#define SOLAR_ASSISTANT_BRIDGE_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include "tuya_cloud_client.h"
#include "solar_assistant_mapping.h"

class SolarAssistantBridge {
public:
    SolarAssistantBridge(TuyaCloudClient& client);
    void handleMqttMessage(const String& topic, const String& payload);
    void processPendingCommands();

private:
    TuyaCloudClient& _tuyaClient;
    struct PendingCmd {
        String code;
        String value;
        bool changed;
        unsigned long lastUpdate;
    };
    // We'll use a map or array based on virtualDeviceMap
    PendingCmd _cache[virtualMapSize];
};

#endif
