#ifndef ALERTS_H
#define ALERTS_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include "compat.h"
#include <vector>
#include "relay_controller.h"

// -----------------------------------------------------------------------------
// Alarms that must not be missed, delivered over two independent paths:
//
//   * Telegram, straight from the board (needs only internet). Token and chat
//     are set from the web UI (stored in NVS); TELEGRAM_BOT_TOKEN /
//     TELEGRAM_CHAT_ID in secrets.h are only defaults.
//   * Home Assistant: every message is also published to dacha_relays/alert
//     on the HA broker; an HA automation turns it into a phone push.
//
//   * Active alarms repeat every kRepeatMs until acknowledged — the button
//     under the Telegram message, the web UI, or (over TLS) the HA app.
//   * When the condition goes away a "resolved" message follows.
//   * Info messages (generator start / stop, reboot) are sent once.
//   * No internet: messages wait in a queue.
//   * /status in the Telegram chat answers with the current state.
//
// HA itself is watched too: an HA automation publishes a heartbeat every
// minute; if it stops (or HA's MQTT LWT says offline, or the HA broker is
// unreachable) the board says so on Telegram — the only path left.
// -----------------------------------------------------------------------------
class Alerts {
public:
    enum Key : uint8_t { Stale = 0, Frozen, VoltEmergency, HaDown, BrokerDown, kCount };
    static constexpr uint32_t kRepeatMs        = 10UL * 60UL * 1000UL;
    // No SOC right after boot is normal; don't call it stale this early.
    static constexpr uint32_t kBootGraceMs     = 5UL * 60UL * 1000UL;
    static constexpr uint32_t kHaHeartbeatMs   = 5UL * 60UL * 1000UL;
    static constexpr uint32_t kBrokerDownMs    = 5UL * 60UL * 1000UL;

    void begin(ResetKind resetKind);
    // ESP8266 has no tasks: call from loop(); does one Telegram step at most
    // once a second. No-op on ESP32 (a FreeRTOS task does it there).
    void service();
    // Main loop: watches the relay controller and HA health, queues messages.
    void tick(const RelayController& relays, bool haMqttConnected);
    bool ack(const String& key, const char* by);

    // HA health inputs (from the HA MQTT client, main loop).
    void onHaHeartbeat();
    void onHaStatus(bool online);
    // Next message for dacha_relays/alert (JSON), if any; drop it once it
    // has been published. Main loop only.
    bool peekHaMessage(String& json) const;
    void dropHaMessage();

    // Telegram configuration from the web UI.
    void setTelegramToken(const String& token);
    void setTelegramChat(const String& chatId);
    void sendTest();

    void fillStatus(JsonObject o) const;
    void fillTelegram(JsonObject o) const;
    void fillHa(JsonObject o) const;

private:
    struct Alarm {
        const char* key;
        bool     active = false;
        bool     acked = false;
        uint32_t sinceMs = 0;
        uint32_t lastSentMs = 0;
        String   detail;
        Alarm(const char* k) : key(k) {}
    };
    Alarm _alarms[kCount] = {{"stale"}, {"frozen"}, {"voltage"}, {"ha_down"}, {"broker_down"}};
    bool  _lastGen = false;
    bool  _genKnown = false;
    ResetKind _resetKind = ResetKind::Other;
    bool  _bootReported = false;

    uint32_t _lastHeartbeatMs = 0;      // 0 = never seen (automation not set up)
    bool     _haOffline = false;
    uint32_t _haDisconnectedSinceMs = 0;
    std::vector<String> _haOut;

    void raise(Key k, const String& detail);
    void clear(Key k);
    void send(const char* key, const char* level, const String& title, const String& text, int ackKey);
    static const char* title(Key k);
    static bool viaHa(Key k) { return k != HaDown && k != BrokerDown; }
};

extern Alerts alerts;

#endif
