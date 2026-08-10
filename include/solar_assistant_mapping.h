#ifndef SOLAR_ASSISTANT_MAPPING_H
#define SOLAR_ASSISTANT_MAPPING_H

#include <Arduino.h>
#include <map>

struct VirtualDpMapping {
    const char* mqttTopic;
    const char* tuyaCode;
    const char* dpId;
};

// Mapping of Solar Assistant MQTT topics to Tuya Virtual Device DP codes and IDs
const VirtualDpMapping virtualDeviceMap[] = {
    {"solar_assistant/total/battery_state_of_charge/state", "battery_percentage", "101"},
    {"solar_assistant/inverter_1/grid_voltage/state", "grid_voltage", "102"},
    {"solar_assistant/inverter_1/pv_power/state", "pv_power", "103"},
    {"solar_assistant/inverter_1/load_power/state", "load_power", "104"},
    {"solar_assistant/inverter_1/grid_power/state", "grid_load", "105"},
    {"solar_assistant/total/battery_power/state", "battery_load", "106"}
};

const int virtualMapSize = sizeof(virtualDeviceMap) / sizeof(virtualDeviceMap[0]);

#endif

