#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <time.h>

#include "secrets.h"
#include "tuya_cloud_client.h"
#include "mppt_bridge.h"
#include "solar_assistant_bridge.h"
#include "solar_manager_protocol.h"

// -----------------------------------------------------------------------------
// Two MQTT brokers:
//   * LAN broker  — where Solar Assistant publishes solar_assistant/**
//                   (subscribe only; used by SolarAssistantBridge)
//   * HA broker   — where Home Assistant + Solar_Manager listen for msbN/**
//                   (publish + subscribe; used by three SolarManagerDevice)
// -----------------------------------------------------------------------------
static WiFiClient   lanTcp;
static PubSubClient mqttLan(lanTcp);
static WiFiClient   haTcp;
static PubSubClient mqttHa(haTcp);

static TuyaCloudClient tuyaClient(TUYA_API_BASE_URL, TUYA_CLIENT_ID, TUYA_CLIENT_SECRET);
static MpptBridge      mpptBridge(tuyaClient);
static SolarAssistantBridge saBridge(tuyaClient);

static SolarManagerDevice smDevices[3] = {
    SolarManagerDevice(MSB_SERIAL_1, "esp32-1.0.0"),
    SolarManagerDevice(MSB_SERIAL_2, "esp32-1.0.0"),
    SolarManagerDevice(MSB_SERIAL_3, "esp32-1.0.0"),
};

// -----------------------------------------------------------------------------
// Reconnect state: non-blocking exponential backoff, capped.
// -----------------------------------------------------------------------------
struct Reconnect {
    uint32_t nextAttemptMs = 0;
    uint32_t backoffMs     = 1000;
};
static Reconnect wifiReconnect;
static Reconnect lanReconnect;
static Reconnect haReconnect;
constexpr uint32_t kBackoffMinMs = 1000;
constexpr uint32_t kBackoffMaxMs = 60000;

static void bumpBackoff(Reconnect& r) {
    r.backoffMs = min(r.backoffMs * 2, kBackoffMaxMs);
    r.nextAttemptMs = millis() + r.backoffMs;
}
static void resetBackoff(Reconnect& r) {
    r.backoffMs = kBackoffMinMs;
    r.nextAttemptMs = 0;
}

// -----------------------------------------------------------------------------
// MQTT dispatch. PubSubClient uses a single callback per client, so we route
// by topic prefix.
// -----------------------------------------------------------------------------
static void onHaMessage(char* topic, byte* payload, unsigned int length) {
    for (auto& dev : smDevices) {
        if (dev.handleMessage(topic, payload, length)) return;
    }
}

static void onLanMessage(char* topic, byte* payload, unsigned int length) {
    String message;
    message.reserve(length);
    for (unsigned int i = 0; i < length; i++) message += (char)payload[i];
    saBridge.handleMqttMessage(String(topic), message);
}

// -----------------------------------------------------------------------------
// WiFi (non-blocking)
// -----------------------------------------------------------------------------
static void kickWifi() {
    uint32_t now = millis();
    if (WiFi.status() == WL_CONNECTED) {
        resetBackoff(wifiReconnect);
        return;
    }
    if (now < wifiReconnect.nextAttemptMs) return;

    Serial.printf("[WiFi] connecting to %s (backoff=%u ms)\n", WIFI_SSID, (unsigned)wifiReconnect.backoffMs);
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    bumpBackoff(wifiReconnect);
}

// -----------------------------------------------------------------------------
// Time (non-blocking check — Tuya signing needs sane epoch)
// -----------------------------------------------------------------------------
static bool timeReady = false;
static void kickTime() {
    if (timeReady) return;
    time_t now = time(nullptr);
    if (now > 1700000000) {   // 2023-11-14
        timeReady = true;
        Serial.println("[Time] synchronized");
    }
}
static void beginNtp() {
    configTime(0, 0, "pool.ntp.org", "time.google.com", "time.nist.gov");
}

// -----------------------------------------------------------------------------
// MQTT connect helpers
// -----------------------------------------------------------------------------
static bool kickLanMqtt() {
    if (WiFi.status() != WL_CONNECTED) return false;
    if (mqttLan.connected()) { resetBackoff(lanReconnect); return true; }
    if (millis() < lanReconnect.nextAttemptMs) return false;

    Serial.printf("[MQTT LAN] connecting %s:%d (backoff=%u)\n",
                  MQTT_LAN_HOST, MQTT_LAN_PORT, (unsigned)lanReconnect.backoffMs);
    mqttLan.setServer(MQTT_LAN_HOST, MQTT_LAN_PORT);
    mqttLan.setCallback(onLanMessage);

    bool ok = mqttLan.connect("esp32-lan", MQTT_LAN_USERNAME, MQTT_LAN_PASSWORD);
    if (!ok) {
        Serial.printf("[MQTT LAN] connect failed rc=%d\n", mqttLan.state());
        bumpBackoff(lanReconnect);
        return false;
    }
    Serial.println("[MQTT LAN] connected");
    resetBackoff(lanReconnect);
    mqttLan.subscribe("solar_assistant/total/#");
    mqttLan.subscribe("solar_assistant/inverter_1/#");
    return true;
}

static bool kickHaMqtt() {
    if (WiFi.status() != WL_CONNECTED) return false;
    if (mqttHa.connected()) { resetBackoff(haReconnect); return true; }
    if (millis() < haReconnect.nextAttemptMs) return false;

    Serial.printf("[MQTT HA] connecting %s:%d (backoff=%u)\n",
                  MQTT_HA_HOST, MQTT_HA_PORT, (unsigned)haReconnect.backoffMs);
    mqttHa.setServer(MQTT_HA_HOST, MQTT_HA_PORT);
    mqttHa.setCallback(onHaMessage);
    mqttHa.setBufferSize(512);

    // Global LWT: single topic can't cover 3 devices, so we use the first as a
    // health marker and each device republishes 'online' on connect. To publish
    // 'offline' per device on ungraceful drop we'd need one MQTT client per
    // device — deferred; a graceful shutdown path (below) covers restarts.
    bool ok = mqttHa.connect(
        "esp32-ha", MQTT_HA_USERNAME, MQTT_HA_PASSWORD,
        (String(MSB_SERIAL_1) + "/online").c_str(), 1, false, "offline"
    );
    if (!ok) {
        Serial.printf("[MQTT HA] connect failed rc=%d\n", mqttHa.state());
        bumpBackoff(haReconnect);
        return false;
    }
    Serial.println("[MQTT HA] connected");
    resetBackoff(haReconnect);
    for (auto& dev : smDevices) {
        dev.setMqttClient(&mqttHa);
        dev.onMqttConnected();
    }
    return true;
}

// -----------------------------------------------------------------------------
// Setup / loop
// -----------------------------------------------------------------------------
void setup() {
    Serial.begin(115200);
    delay(50);
    Serial.println("\n[Boot] ESP32 Tuya <-> Solar_Manager bridge");

    kickWifi();
    beginNtp();
}

void loop() {
    kickWifi();
    kickTime();

    bool lanOk = kickLanMqtt();
    bool haOk  = kickHaMqtt();

    if (lanOk) mqttLan.loop();
    if (haOk)  mqttHa.loop();

    if (!timeReady) return; // Tuya signing needs epoch

    static uint32_t lastMpptPoll = 0;
    if (millis() - lastMpptPoll > 10000) {
        lastMpptPoll = millis();
        mpptBridge.updateAll();
    }

    static uint32_t lastSaSync = 0;
    if (lanOk && millis() - lastSaSync > 10000) {
        lastSaSync = millis();
        saBridge.processPendingCommands();
    }

    if (haOk) {
        for (int i = 0; i < 3; i++) {
            smDevices[i].tick(mpptBridge.getState(i));
        }
    }
}
