#ifndef HA_RELAYS_H
#define HA_RELAYS_H

#include <Arduino.h>
#include <PubSubClient.h>
#include "relay_controller.h"

// -----------------------------------------------------------------------------
// Exposes the relay controller to Home Assistant via MQTT discovery on the HA
// broker: one JSON state topic, entities pick fields with value_template.
//
// Commands (generator mode, season mode) are only accepted when the HA broker
// connection is TLS — otherwise anyone who sniffs the plaintext credentials
// could start the generator. Without TLS the same values are published as
// read-only sensors.
// -----------------------------------------------------------------------------
class HaRelays {
public:
    HaRelays(RelayController& relays) : _relays(relays) {}

    void begin(PubSubClient* mqtt, bool commandsEnabled);
    void onMqttConnected();
    // Returns true if the topic was one of ours.
    bool handleMessage(const char* topic, const uint8_t* payload, unsigned int length);
    void tick();

private:
    void publishDiscovery();
    void publishState();
    void publishEntity(const char* component, const char* id, const char* name,
                       const char* valueTemplate, const char* extraJson);

    RelayController& _relays;
    PubSubClient*    _mqtt = nullptr;
    bool             _commandsEnabled = false;
    uint32_t         _lastPublishMs = 0;
    uint32_t         _lastChange = 0;
};

#endif
