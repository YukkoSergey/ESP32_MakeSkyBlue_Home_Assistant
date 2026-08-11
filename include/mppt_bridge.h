#ifndef MPPT_BRIDGE_H
#define MPPT_BRIDGE_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include "tuya_cloud_client.h"
#include "secrets.h"

// -----------------------------------------------------------------------------
// MpptState holds RAW Modbus register values, matching the units Solar_Manager
// expects on <sn>/notify. The JSON protocol in Solar_Manager assigns a scale to
// each register (e.g. battery_voltage scale=0.1, meaning raw 532 -> 53.2 V);
// we store the raw side and let Solar_Manager multiply.
//
// UINT16 unless noted. Signed values are stored in int16_t and repacked as
// two's-complement when the notify frame is built.
// -----------------------------------------------------------------------------
struct MpptState {
    // FC4 input registers (Solar_Manager keys 0x40xxxx)
    uint16_t faultStatus;        // 0x400001 — enum
    uint16_t batteryVoltageRaw;  // 0x400005 — scale 0.1 V
    int16_t  batteryCurrentRaw;  // 0x400006 — INT16, scale 0.1 A
    uint16_t pvVoltageRaw;       // 0x400007 — scale 0.1 V
    uint16_t chargePowerRaw;     // 0x400008 — scale 1 W
    int16_t  temperatureRaw;     // 0x400009 — INT16, scale 0.1 C
    uint16_t cumulativeGenRaw;   // 0x40000A — scale 1 kWh
    uint16_t outCurrentRaw;      // 0x40000B — scale 0.1 A (not published by Tuya)
    uint16_t workStatusRaw;      // 0x40000C — enum 0..6
    uint16_t dailyGenRaw;        // 0x40001A — scale 0.1 kWh (derived)

    // FC3 holding registers (Solar_Manager keys 0x30xxxx)
    uint16_t equalizationVoltRaw; // 0x300001 — scale 0.1 V
    uint16_t floatVoltRaw;        // 0x300002 — scale 0.1 V
    uint16_t outTimeSetRaw;       // 0x300003 — hours
    uint16_t chargeCurrentRaw;    // 0x300004 — scale 0.1 A
    uint16_t lowVoltRaw;          // 0x300005 — scale 0.1 V
    uint16_t recoveryVoltRaw;     // 0x300006 — scale 0.1 V
    uint16_t commAddress;         // 0x300007
    uint16_t batteryTypeRaw;      // 0x300008 — enum 0/1
    uint16_t batteryCells;        // 0x300009
    uint16_t calibVoltRaw;        // 0x30000A

    bool valid;                   // true after at least one successful poll
};

class MpptBridge {
public:
    struct Device {
        const char* tuyaDeviceId;
    };

    MpptBridge(TuyaCloudClient& client);
    void updateAll();
    const MpptState& getState(int index) const;

    // First few minutes after boot, updateAll() serves pre-seeded realistic
    // values instead of hitting Tuya. Lets the HA card show non-zero data
    // immediately (esp. at night when real DPs are all zero). Value in ms.
    static constexpr uint32_t kFakeSeedWindowMs = 180000; // 3 min

private:
    TuyaCloudClient& _tuyaClient;
    Device    _devices[3];
    MpptState _states[3];

    // Daily generation tracking. Tuya electric_total is in 0.1 kWh; the
    // delta since midnight is stored directly as dailyGenRaw (SM 0x40001A
    // scale=0.1 kWh — same unit, no conversion needed).
    uint16_t _dailyBaseline[3];  // electric_total raw at start of UTC day
    int      _lastYday[3];       // day-of-year when baseline was captured
    bool     _baselineSet[3];    // false until first successful poll

    bool updateOne(int index);
    void seedRealistic();
};

#endif
