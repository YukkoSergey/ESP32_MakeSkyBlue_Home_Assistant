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
        _dailyBaseline[i] = 0;
        _lastYday[i]      = -1;
        _baselineSet[i]   = false;
    }
    seedRealistic();
}

// Pre-seeded plausible daylight values used during the first
// kFakeSeedWindowMs after boot. Added during initial development to allow
// testing the HA card at night when real Tuya DPs (pv_voltage, bat_current,
// charge_power) are legitimately zero and the card would appear dead.
// After the seed window expires, real Tuya data takes over and overwrites
// these values. The numbers match the mock emulator (tools/mock_makeskyblue.py)
// and approximate a typical summer afternoon on a 48 V / 4-cell LiFePO4 system.
void MpptBridge::seedRealistic() {
    // Cumulative generation approximated from real device values at debug time
    // (Tuya raw / 10 to match SM register scale=1 kWh): 771 / 603 / 631 kWh.
    const uint16_t cumSeed[3] = {772, 603, 632};
    for (int i = 0; i < 3; i++) {
        MpptState s{};
        s.faultStatus        = 0;                        // normal, no fault
        s.batteryVoltageRaw  = 532;                      // 53.2 V — battery near full
        s.batteryCurrentRaw  = 187;                      // 18.7 A — charging
        s.pvVoltageRaw       = 1453;                     // 145.3 V — mid-day PV
        s.chargePowerRaw     = (uint16_t)(1045 + i*40); // 1045/1085/1125 W — slightly different per controller
        s.temperatureRaw     = (int16_t)(341 + i*3);    // 34.1/34.4/34.7 C
        s.cumulativeGenRaw   = cumSeed[i];               // lifetime kWh
        s.outCurrentRaw      = 196;                      // 19.6 A output
        s.workStatusRaw      = 4;                        // mppt_tracking
        s.dailyGenRaw        = 47;                       // 4.7 kWh today

        s.equalizationVoltRaw = 588;                     // 58.8 V
        s.floatVoltRaw        = 546;                     // 54.6 V
        s.outTimeSetRaw       = 1;                       // 1 h
        s.chargeCurrentRaw    = 400;                     // 40.0 A limit
        s.lowVoltRaw          = 440;                     // 44.0 V cutoff
        s.recoveryVoltRaw     = 480;                     // 48.0 V recovery
        s.commAddress         = 1;
        s.batteryTypeRaw      = 1;                       // lithium
        s.batteryCells        = 4;
        s.calibVoltRaw        = 0;

        s.valid = true;
        _states[i] = s;
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

    // Heap-allocated: a StaticJsonDocument<4096> on the loopTask stack (8 KB
    // by default) leaves no room for HTTPS/mbedTLS scratch buffers during the
    // same call and blew the canary in the field. Even with the 16 KB stack
    // bump we prefer heap for anything this big.
    DynamicJsonDocument doc(6144);
    DeserializationError err = deserializeJson(doc, props);
    if (err) {
        Serial.printf("[MpptBridge] JSON parse failed for MPPT %d: %s\n", index, err.c_str());
        return false;
    }

    // Legacy iot-03 status returns result directly as an array of {code,value}.
    JsonArrayConst arr = doc["result"].as<JsonArrayConst>();
    if (arr.isNull()) {
        Serial.printf("[MpptBridge] MPPT %d: result is not an array (success=%d)\n",
                      index, (int)(doc["success"] | false));
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

    // electric_total: Tuya raw unit is 0.1 kWh (e.g. 7715 = 771.5 kWh).
    // Solar_Manager register 0x40000A has scale=1 kWh, so divide by 10.
    long etRaw = 0;
    JsonVariantConst etv;
    if (findProp(arr, "electric_total", etv) && !etv.isNull()) etRaw = etv.as<long>();
    next.cumulativeGenRaw = (uint16_t)(etRaw / 10);

    // Daily generation: delta from midnight baseline using the same 0.1 kWh
    // raw unit as electric_total. SM register 0x40001A has scale=0.1 kWh so
    // the value passes through unchanged — no extra conversion needed.
    {
        time_t now = time(nullptr);
        struct tm* t = localtime(&now);
        int yday = t ? t->tm_yday : -1;

        if (!_baselineSet[index] || yday != _lastYday[index]) {
            // First poll ever, or day rolled over — reset baseline.
            _dailyBaseline[index] = (uint16_t)etRaw;
            _lastYday[index]      = yday;
            _baselineSet[index]   = true;
        }

        long delta = etRaw - (long)_dailyBaseline[index];
        if (delta < 0) delta = 0;          // counter wrap / clock jump guard
        if (delta > 0xFFFF) delta = 0xFFFF;
        next.dailyGenRaw = (uint16_t)delta;
    }

    next.outCurrentRaw       = 0;    // not exposed by Tuya
    next.workStatusRaw       = mapChargeMode(arr);

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
    // First kFakeSeedWindowMs after boot: serve the seed, don't touch Tuya.
    static bool announcedReal = false;
    if (millis() < kFakeSeedWindowMs) {
        static bool announcedFake = false;
        if (!announcedFake) {
            Serial.printf("[MpptBridge] seeded values (fake) for first %u s\n",
                          (unsigned)(kFakeSeedWindowMs / 1000));
            announcedFake = true;
        }
        return;
    }
    if (!announcedReal) {
        Serial.println("[MpptBridge] seed window over, polling Tuya");
        announcedReal = true;
    }
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
