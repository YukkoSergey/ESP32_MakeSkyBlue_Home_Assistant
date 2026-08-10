#ifndef MPPT_BRIDGE_H
#define MPPT_BRIDGE_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include "tuya_cloud_client.h"
#include "secrets.h"

struct MpptState {
    float pvVoltage;
    float batteryVoltage;
    float batteryCurrent;
    float power;
    float temperature;
    float totalEnergy;
    uint16_t fault;
    String chargeMode;
    float equalizationVoltage;
    float floatVoltage;
    float lowVoltage;
    float recoveryVoltage;
    int chargeCurrent;
    String batteryType;
    int batteryCells;
    int outputTimer;
};

struct SolarDevice {
    const char* id;
    const char* mqttPrefix;
};

class MpptBridge {
public:
    MpptBridge(TuyaCloudClient& client);
    void updateAll();
    MpptState getState(int index);

private:
    TuyaCloudClient& _tuyaClient;
    SolarDevice _devices[3];
    MpptState _states[3];
    float getPropertyValue(JsonDocument& doc, const char* code);
};

#endif
