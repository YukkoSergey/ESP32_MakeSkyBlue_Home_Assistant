#include "solar_manager_protocol.h"
#include <ArduinoJson.h>
#include <WiFi.h>

SolarManagerDevice::SolarManagerDevice(const char* serial, const char* firmwareVersion)
    : _serial(serial), _firmwareVersion(firmwareVersion) {
    resetDefaultSegments();
}

void SolarManagerDevice::resetDefaultSegments() {
    // Defaults from custom_components/solar_manager/device_protocol/makeskybluemppt.json
    _segments[0] = {1, 4, 1, 12};   // FC4 input regs 1..12
    _segments[1] = {1, 4, 26, 1};   // FC4 input reg 26
    _segments[2] = {1, 3, 1, 10};   // FC3 holding regs 1..10
    _segmentCount = 3;
}

String SolarManagerDevice::topic(const char* suffix) const {
    String t = _serial;
    t += "/";
    t += suffix;
    return t;
}

void SolarManagerDevice::putBE16(uint8_t* out, uint16_t v) {
    out[0] = (v >> 8) & 0xFF;
    out[1] = v & 0xFF;
}

bool SolarManagerDevice::armLastWill(PubSubClient& client) {
    // PubSubClient's LWT is set through connect(), not on the client object,
    // so this method is a no-op — the caller passes willTopic/willMessage
    // directly to client.connect(). Kept for API symmetry / future use.
    (void)client;
    return true;
}

void SolarManagerDevice::onMqttConnected() {
    if (!_mqtt) return;
    _mqtt->subscribe(topic("config").c_str());
    _mqtt->subscribe(topic("control/#").c_str());
    _mqtt->subscribe(topic("host/#").c_str());
    _onlinePublished = false;   // force publishOnline on next tick
    publishOnline();
    Serial.printf("[SM %s] connected, subscribed to config/control/host\n", _serial);
}

void SolarManagerDevice::publishOnline() {
    if (!_mqtt || !_mqtt->connected()) return;
    _mqtt->publish(topic("online").c_str(), _firmwareVersion);
    _onlinePublished = true;
    Serial.printf("[SM %s] online -> %s\n", _serial, _firmwareVersion);
}

void SolarManagerDevice::publishDiagnostics() {
    if (!_mqtt || !_mqtt->connected()) return;
    StaticJsonDocument<128> doc;
    doc["ssid"] = WiFi.SSID();
    doc["rssi"] = WiFi.RSSI();
    doc["led"]  = _ledOn ? "on" : "off";
    char payload[128];
    size_t n = serializeJson(doc, payload, sizeof(payload));
    _mqtt->publish(topic("diagnostics").c_str(), (const uint8_t*)payload, n, false);
}

// -----------------------------------------------------------------------------
// Notify frame packing. Register value selection follows the map worked out
// against makeskybluemppt.json:
//   FC4:  0x400001 fault / 0x400005 batV / 0x400006 batI(INT16) / 0x400007 pvV /
//         0x400008 power / 0x400009 tempC(INT16) / 0x40000A cum / 0x40000B outI /
//         0x40000C workStatus / 0x40001A daily
//   FC3:  0x300001 eqV / 0x300002 floatV / 0x300003 outTime / 0x300004 chargeI /
//         0x300005 lowV / 0x300006 recoverV / 0x300007 commAddr / 0x300008 batType /
//         0x300009 cells / 0x30000A calibV
// -----------------------------------------------------------------------------

static uint16_t reencodeSigned(int16_t v) {
    return (uint16_t)v; // two's complement round-trip
}

static uint16_t pickFC4Register(uint16_t addr, const MpptState& s) {
    switch (addr) {
        case 0x0001: return s.faultStatus;
        case 0x0005: return s.batteryVoltageRaw;
        case 0x0006: return reencodeSigned(s.batteryCurrentRaw);
        case 0x0007: return s.pvVoltageRaw;
        case 0x0008: return s.chargePowerRaw;
        case 0x0009: return reencodeSigned(s.temperatureRaw);
        case 0x000A: return s.cumulativeGenRaw;
        case 0x000B: return s.outCurrentRaw;
        case 0x000C: return s.workStatusRaw;
        case 0x001A: return s.dailyGenRaw;
        default:     return 0;
    }
}

static uint16_t pickFC3Register(uint16_t addr, const MpptState& s) {
    switch (addr) {
        case 0x0001: return s.equalizationVoltRaw;
        case 0x0002: return s.floatVoltRaw;
        case 0x0003: return s.outTimeSetRaw;
        case 0x0004: return s.chargeCurrentRaw;
        case 0x0005: return s.lowVoltRaw;
        case 0x0006: return s.recoveryVoltRaw;
        case 0x0007: return s.commAddress;
        case 0x0008: return s.batteryTypeRaw;
        case 0x0009: return s.batteryCells;
        case 0x000A: return s.calibVoltRaw;
        default:     return 0;
    }
}

void SolarManagerDevice::publishNotifyFrame(const SmSegment& seg, const MpptState& state) {
    // Header 6 bytes + N regs * 2 bytes. PubSubClient default buffer is small;
    // segments here top out at 12 regs (30 bytes), safely under any sane limit.
    uint8_t buf[64];
    const uint16_t total = 6 + (uint16_t)seg.length * 2;
    if (total > sizeof(buf)) return;

    buf[0] = seg.slaveId;
    buf[1] = seg.function;
    putBE16(&buf[2], seg.startAddress);
    putBE16(&buf[4], seg.length);

    for (uint16_t i = 0; i < seg.length; i++) {
        uint16_t addr = seg.startAddress + i;
        uint16_t v = 0;
        if (seg.function == 4) v = pickFC4Register(addr, state);
        else if (seg.function == 3) v = pickFC3Register(addr, state);
        putBE16(&buf[6 + i * 2], v);
    }

    _mqtt->publish(topic("notify").c_str(), buf, total, false);
}

void SolarManagerDevice::publishNotifyAll(const MpptState& state) {
    if (!_mqtt || !_mqtt->connected()) return;
    if (!state.valid) return;  // don't publish zeroed placeholder as "real"
    for (uint8_t i = 0; i < _segmentCount; i++) {
        publishNotifyFrame(_segments[i], state);
    }
}

void SolarManagerDevice::tick(const MpptState& state) {
    if (!_mqtt) return;
    uint32_t now = millis();

    if (_mqtt->connected() && !_onlinePublished) {
        publishOnline();
    }

    if (now - _lastNotifyMs >= kNotifyIntervalMs) {
        _lastNotifyMs = now;
        publishNotifyAll(state);
    }
    if (now - _lastDiagMs >= kDiagIntervalMs) {
        _lastDiagMs = now;
        publishDiagnostics();
    }
}

// -----------------------------------------------------------------------------
// Inbound: /config, /control/*, /host/*
// -----------------------------------------------------------------------------

void SolarManagerDevice::handleConfig(const uint8_t* payload, unsigned int length) {
    StaticJsonDocument<512> doc;
    if (deserializeJson(doc, payload, length) != DeserializationError::Ok) {
        Serial.printf("[SM %s] /config: bad JSON\n", _serial);
        return;
    }
    JsonArrayConst segs = doc["segments"].as<JsonArrayConst>();
    if (segs.isNull()) return;

    uint8_t n = 0;
    for (JsonObjectConst s : segs) {
        if (n >= 5) break;
        _segments[n].slaveId      = s["slave_id"]      | (uint8_t)1;
        _segments[n].function     = s["read_command"]  | (uint8_t)4;
        _segments[n].startAddress = s["start_address"] | (uint16_t)0;
        _segments[n].length       = s["length"]        | (uint16_t)0;
        n++;
    }
    _segmentCount = n;
    Serial.printf("[SM %s] /config: %u segments\n", _serial, (unsigned)n);
}

void SolarManagerDevice::handleControlCmd(const uint8_t* payload, unsigned int length) {
    // Binary Modbus write from HA. Decode and log only — WRITE DISABLED IN MVP.
    // Layout (per modbus_protocol_helper.pack_data):
    //   fn 5:  [slave][5][addr:2][coil:2]        coil=0xFF00 on / 0x0000 off
    //   fn 6:  [slave][6][addr:2][val:2]         single holding register
    //   fn 16: [slave][16][addr:2][count:2][values...]
    if (length < 6) {
        Serial.printf("[SM %s] /control/cmd: too short (%u B)\n", _serial, length);
        return;
    }
    uint8_t slave = payload[0];
    uint8_t fn    = payload[1];
    uint16_t addr = (payload[2] << 8) | payload[3];
    uint16_t v    = (payload[4] << 8) | payload[5];

    if (fn == 5) {
        Serial.printf("[SM %s] WRITE DISABLED: fn5 coil slave=%u addr=0x%04X -> %s\n",
                      _serial, slave, addr, v == 0xFF00 ? "ON" : "OFF");
    } else if (fn == 6) {
        Serial.printf("[SM %s] WRITE DISABLED: fn6 holding slave=%u addr=0x%04X val=%u\n",
                      _serial, slave, addr, v);
    } else if (fn == 16) {
        Serial.printf("[SM %s] WRITE DISABLED: fn16 multi slave=%u addr=0x%04X count=%u\n",
                      _serial, slave, addr, v);
    } else {
        Serial.printf("[SM %s] WRITE DISABLED: unknown fn=%u\n", _serial, fn);
    }
}

void SolarManagerDevice::handleControlLed(const uint8_t* payload, unsigned int length) {
    if (length == 2 && payload[0] == 'o' && payload[1] == 'n') {
        _ledOn = true;
    } else if (length == 3 && payload[0] == 'o' && payload[1] == 'f' && payload[2] == 'f') {
        _ledOn = false;
    } else {
        Serial.printf("[SM %s] /control/led: unrecognized payload (%u B)\n", _serial, length);
        return;
    }
    Serial.printf("[SM %s] LED -> %s\n", _serial, _ledOn ? "on" : "off");
    publishDiagnostics(); // reflect change immediately
}

bool SolarManagerDevice::handleMessage(const char* topicIn, const uint8_t* payload, unsigned int length) {
    const size_t slen = strlen(_serial);
    if (strncmp(topicIn, _serial, slen) != 0 || topicIn[slen] != '/') {
        return false;
    }
    const char* sub = topicIn + slen + 1;

    if (strcmp(sub, "config") == 0) {
        handleConfig(payload, length);
    } else if (strcmp(sub, "control/cmd") == 0) {
        handleControlCmd(payload, length);
    } else if (strcmp(sub, "control/led") == 0) {
        handleControlLed(payload, length);
    } else if (strcmp(sub, "control/restart") == 0) {
        Serial.printf("[SM %s] /control/restart — DISABLED IN MVP\n", _serial);
    } else if (strcmp(sub, "control/reconfig") == 0) {
        Serial.printf("[SM %s] /control/reconfig — resetting segments, republishing online\n", _serial);
        resetDefaultSegments();
        publishOnline();
    } else if (strcmp(sub, "host/heartbeat") == 0) {
        _lastHeartbeatMs = millis();
    } else {
        // subscribed via /control/# and /host/# but not one of the above
        Serial.printf("[SM %s] unhandled subtopic '%s' (%u B)\n", _serial, sub, length);
    }
    return true;
}
