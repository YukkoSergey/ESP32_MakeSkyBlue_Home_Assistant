#include "mppt_bridge.h"

// -----------------------------------------------------------------------------
// Tuya returns each DP as a JSON number in its own device-native units, which
// happen to line up with the raw Modbus register values Solar_Manager expects.
//
// Examples (spec):
//   bat_voltage    Tuya raw 532   -> Solar_Manager register 532 (scale 0.1 = 53.2 V)
//   bat_current    Tuya raw 187   -> Solar_Manager register 187 (INT16, scale 0.1)
//   pv_voltage     Tuya raw 1453  -> Solar_Manager register 1453 (scale 0.1 = 145.3 V)
//   temp_current   Tuya raw 341   -> Solar_Manager register 341 (INT16, scale 0.1 = 34.1 C)
//   charge_power   Tuya raw 10452 -> Solar_Manager register 1045 (scale 1 W, need /10)
//   constant_cur_set Tuya raw 40 A -> Solar_Manager register 400 (scale 0.1 A, need *10)
//
// So most DPs are copied straight through; charge_power is divided by 10,
// constant_cur_set is multiplied by 10.
// -----------------------------------------------------------------------------

MpptBridge::MpptBridge(TuyaCloudClient& client) : _tuyaClient(client) {
    _devices[0] = {TUYA_MPPT_1_ID};
    _devices[1] = {TUYA_MPPT_2_ID};
    _devices[2] = {TUYA_MPPT_3_ID};
    for (int i = 0; i < 3; i++) {
        _states[i] = MpptState{};
    }
}

static bool findProp(JsonArrayConst props, const char* code, JsonVariantConst& out) {
    for (JsonObjectConst p : props) {
        const char* c = p["code"];
        if (c && strcmp(c, code) == 0) {
            out = p["value"];
            return true;
        }
    }
    return false;
}

static uint16_t propU16(JsonArrayConst props, const char* code, uint16_t fallback = 0) {
    JsonVariantConst v;
    if (!findProp(props, code, v) || v.isNull()) return fallback;
    long n = v.as<long>();
    if (n < 0) n = 0;
    if (n > 0xFFFF) n = 0xFFFF;
    return (uint16_t)n;
}

static int16_t propI16(JsonArrayConst props, const char* code, int16_t fallback = 0) {
    JsonVariantConst v;
    if (!findProp(props, code, v) || v.isNull()) return fallback;
    long n = v.as<long>();
    if (n < -32768) n = -32768;
    if (n > 32767)  n = 32767;
    return (int16_t)n;
}

static uint16_t mapBatteryType(JsonArrayConst props) {
    JsonVariantConst v;
    if (!findProp(props, "battery_type", v) || v.isNull()) return 0;
    const char* s = v.as<const char*>();
    if (!s) {
        long n = v.as<long>();
        return (uint16_t)(n & 0xFFFF);
    }
    if (strcasecmp(s, "lithium") == 0 || strcasecmp(s, "li") == 0) return 1;
    return 0; // lead_acid / unknown -> 0
}

static uint16_t mapChargeMode(JsonArrayConst props) {
    // TODO: chargeMode mapping table (Step 4) — for now log & return 0.
    JsonVariantConst v;
    if (!findProp(props, "charge_mode", v) || v.isNull()) return 0;
    const char* s = v.as<const char*>();
    if (s && s[0]) {
        Serial.printf("[MpptBridge] charge_mode raw='%s' (mapping TODO)\n", s);
    }
    return 0;
}

bool MpptBridge::updateOne(int index) {
    String props = _tuyaClient.getDeviceProperties(_devices[index].tuyaDeviceId);
    if (props.length() == 0) return false;

    StaticJsonDocument<4096> doc;
    DeserializationError err = deserializeJson(doc, props);
    if (err) {
        Serial.printf("[MpptBridge] JSON parse failed for MPPT %d: %s\n", index, err.c_str());
        return false;
    }

    JsonArrayConst arr = doc["result"]["properties"].as<JsonArrayConst>();
    if (arr.isNull()) {
        Serial.printf("[MpptBridge] MPPT %d: missing result.properties\n", index);
        return false;
    }

    // Build fresh state locally, then atomically assign — a bad partial poll
    // won't overwrite last-known-good values in _states[index].
    MpptState next{};

    next.faultStatus         = propU16(arr, "fault");
    next.batteryVoltageRaw   = propU16(arr, "bat_voltage");
    next.batteryCurrentRaw   = propI16(arr, "bat_current");
    next.pvVoltageRaw        = propU16(arr, "pv_voltage");

    // charge_power: Tuya raw is tenths of a watt, Solar_Manager register is 1 W
    long powerRaw = 0;
    JsonVariantConst pv;
    if (findProp(arr, "power", pv) && !pv.isNull()) {
        powerRaw = pv.as<long>();
    }
    long powerW = powerRaw / 10;
    if (powerW < 0) powerW = 0;
    if (powerW > 0xFFFF) powerW = 0xFFFF;
    next.chargePowerRaw = (uint16_t)powerW;

    next.temperatureRaw      = propI16(arr, "temp_current");
    next.cumulativeGenRaw    = propU16(arr, "electric_total");
    next.outCurrentRaw       = 0;    // not exposed by Tuya
    next.workStatusRaw       = mapChargeMode(arr);
    next.dailyGenRaw         = 0;    // TODO: derive from cumulative delta

    next.equalizationVoltRaw = propU16(arr, "equalization_volt");
    next.floatVoltRaw        = propU16(arr, "float_charg_volt");
    next.outTimeSetRaw       = propU16(arr, "output_timr");

    // constant_cur_set: Tuya raw is whole amps, Solar_Manager register is 0.1 A
    long ampsRaw = 0;
    if (findProp(arr, "constant_cur_set", pv) && !pv.isNull()) {
        ampsRaw = pv.as<long>();
    }
    long chargeCur = ampsRaw * 10;
    if (chargeCur < 0) chargeCur = 0;
    if (chargeCur > 0xFFFF) chargeCur = 0xFFFF;
    next.chargeCurrentRaw = (uint16_t)chargeCur;

    next.lowVoltRaw          = propU16(arr, "undervol_value");
    next.recoveryVoltRaw     = propU16(arr, "uv_reco_value");
    next.commAddress         = 1;
    next.batteryTypeRaw      = mapBatteryType(arr);
    next.batteryCells        = propU16(arr, "battery_cell");
    next.calibVoltRaw        = 0;

    next.valid = true;
    _states[index] = next;
    return true;
}

void MpptBridge::updateAll() {
    for (int i = 0; i < 3; i++) {
        if (!updateOne(i)) {
            Serial.printf("[MpptBridge] update MPPT %d failed (keeping last state)\n", i);
        }
    }
}

const MpptState& MpptBridge::getState(int index) const {
    static const MpptState empty{};
    if (index < 0 || index >= 3) return empty;
    return _states[index];
}
