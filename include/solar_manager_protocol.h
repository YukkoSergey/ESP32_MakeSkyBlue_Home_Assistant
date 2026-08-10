#ifndef SOLAR_MANAGER_PROTOCOL_H
#define SOLAR_MANAGER_PROTOCOL_H

#include <Arduino.h>
#include <PubSubClient.h>
#include "sm_config.h"
#include "mppt_bridge.h"

// -----------------------------------------------------------------------------
// SolarManagerDevice emulates one MakeSkyBlue MPPT device on the MQTT wire in
// the format the Solar_Manager Home Assistant integration expects.
//
// Wire protocol (see maybetaken/Solar_Manager base_device.py + MakeSkyBlueMppt):
//
//   Published by us:
//     <sn>/online        firmware version string
//     <sn>/diagnostics   JSON {"ssid", "rssi", "led"}
//     <sn>/notify        binary TLD [slave:1][fn:1][start:2 BE][len:2 BE][data]
//
//   Subscribed by us:
//     <sn>/config              JSON {"segments":[{slave_id,start_address,length,read_command}]}
//     <sn>/control/cmd         binary Modbus write (fn 5/6/16) — logged, WRITE DISABLED
//     <sn>/control/led         "on"|"off"
//     <sn>/control/restart     "restart"                                — logged, DISABLED
//     <sn>/control/reconfig    "reconfig"                               — re-send config
//     <sn>/host/heartbeat      "alive" (HA -> device, 5 s)              — timestamp only
//
// Notify publishes every notifyIntervalMs, diagnostics every diagIntervalMs.
// HA clears data if notify is silent for 120 s (base_device.CLEAR_INTERVAL),
// so 5 s / 30 s keeps us well within that window.
// -----------------------------------------------------------------------------

class SolarManagerDevice {
public:
    SolarManagerDevice(const char* serial, const char* firmwareVersion);

    void setMqttClient(PubSubClient* mqtt) { _mqtt = mqtt; }

    // Subscribe to <sn>/config, /control/#, /host/# and publish <sn>/online.
    // Call on every successful MQTT (re)connect.
    void onMqttConnected();

    // Set the last-will payload on the given client for THIS device before the
    // client connects. Payload matches base_device.py expectation ("offline").
    // Returns true if the client accepted the LWT.
    bool armLastWill(PubSubClient& client);

    // Route a message received from the HA broker; returns true if handled.
    bool handleMessage(const char* topic, const uint8_t* payload, unsigned int length);

    // Called by main every loop; publishes notify + diagnostics on their own
    // timers. mpptState is the current data snapshot for this device.
    void tick(const MpptState& state);

    const char* serial() const { return _serial; }

private:
    const char*    _serial;
    const char*    _firmwareVersion;
    PubSubClient*  _mqtt = nullptr;

    // Segments HA can override via <sn>/config. Defaults come from
    // makeskybluemppt.json.
    SmSegment      _segments[5];
    uint8_t        _segmentCount = 0;

    bool           _ledOn = true;
    uint32_t       _lastNotifyMs = 0;
    uint32_t       _lastDiagMs   = 0;
    uint32_t       _lastHeartbeatMs = 0;
    bool           _onlinePublished = false;

    static constexpr uint32_t kNotifyIntervalMs = 5000;
    static constexpr uint32_t kDiagIntervalMs   = 30000;

    void resetDefaultSegments();
    void publishOnline();
    void publishDiagnostics();
    void publishNotifyAll(const MpptState& state);
    void publishNotifyFrame(const SmSegment& seg, const MpptState& state);
    void handleConfig(const uint8_t* payload, unsigned int length);
    void handleControlCmd(const uint8_t* payload, unsigned int length);
    void handleControlLed(const uint8_t* payload, unsigned int length);

    String topic(const char* suffix) const;
    static void putBE16(uint8_t* out, uint16_t v);
};

#endif
