#include "mppt_bridge.h"

MpptBridge::MpptBridge(TuyaCloudClient& client) : _tuyaClient(client) {
    _devices[0] = {TUYA_MPPT_1_ID, "solar_assistant/mppt1"};
    _devices[1] = {TUYA_MPPT_2_ID, "solar_assistant/mppt2"};
    _devices[2] = {TUYA_MPPT_3_ID, "solar_assistant/mppt3"};
}

float MpptBridge::getPropertyValue(JsonDocument& doc, const char* code) {
    JsonArray props = doc["result"]["properties"];
    for (JsonObject prop : props) {
        if (prop["code"] == code) {
            return prop["value"].as<float>();
        }
    }
    return 0.0f;
}

void MpptBridge::updateAll() {
    for (int i = 0; i < 3; i++) {
        String props = _tuyaClient.getDeviceProperties(_devices[i].id);
        if (props == "") continue;
        StaticJsonDocument<4096> doc;
        if (deserializeJson(doc, props) != DeserializationError::Ok) continue;

        _states[i].pvVoltage = getPropertyValue(doc, "pv_voltage");
        _states[i].batteryVoltage = getPropertyValue(doc, "bat_voltage");
        _states[i].batteryCurrent = getPropertyValue(doc, "bat_current");
        _states[i].power = getPropertyValue(doc, "power");
        _states[i].temperature = getPropertyValue(doc, "temp_current");
        _states[i].totalEnergy = getPropertyValue(doc, "electric_total");

        // Map other properties if available
        JsonArray propsArray = doc["result"]["properties"];
        for (JsonObject prop : propsArray) {
            const char* code = prop["code"];
            if (strcmp(code, "fault") == 0) _states[i].fault = prop["value"].as<uint16_t>();
            else if (strcmp(code, "charge_mode") == 0) _states[i].chargeMode = prop["value"].as<String>();
            else if (strcmp(code, "equalization_volt") == 0) _states[i].equalizationVoltage = prop["value"].as<float>();
            else if (strcmp(code, "float_charg_volt") == 0) _states[i].floatVoltage = prop["value"].as<float>();
            else if (strcmp(code, "output_timr") == 0) _states[i].outputTimer = prop["value"].as<int>();
            else if (strcmp(code, "constant_cur_set") == 0) _states[i].chargeCurrent = prop["value"].as<int>();
            else if (strcmp(code, "undervol_value") == 0) _states[i].lowVoltage = prop["value"].as<float>();
            else if (strcmp(code, "uv_reco_value") == 0) _states[i].recoveryVoltage = prop["value"].as<float>();
            else if (strcmp(code, "battery_type") == 0) _states[i].batteryType = prop["value"].as<String>();
            else if (strcmp(code, "battery_cell") == 0) _states[i].batteryCells = prop["value"].as<int>();
        }
    }
}

MpptState MpptBridge::getState(int index) {
    if (index < 0 || index >= 3) return MpptState();
    return _states[index];
}

