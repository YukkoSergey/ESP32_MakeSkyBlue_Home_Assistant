// -----------------------------------------------------------------------------
// NodeMCU (ESP8266) build: relays only — the board in the shed that drives the
// heavy-load contactor and the generator autostart. Same relay / settings /
// alerts / web / HA code as the ESP32 bridge (see compat.h); no Tuya bridge
// (too heavy for an ESP8266 — that belongs on the VPS).
//
//   pio run -e nodemcu -t upload          (USB, first time)
//   pio run -e nodemcu_ota -t upload      (over WiFi afterwards)
// -----------------------------------------------------------------------------
#if defined(ESP8266)

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <ArduinoOTA.h>
#include <time.h>

#include "secrets.h"
#include "compat.h"
#include "app_log.h"
#include "settings.h"
#include "relay_controller.h"
#include "ha_relays.h"
#include "alerts.h"
#include "web_ui.h"
#include "solar_assistant_mapping.h"

static const char* kFirmwareBuild = __DATE__ " " __TIME__;
static const char* kHostname = "dacha-relays";
static const char* kTimeZone = "EET-2EEST,M3.5.0/3,M10.5.0/4";   // Kyiv

static WiFiClient   lanTcp;
static PubSubClient mqttLan(lanTcp);
static WiFiClient   haTcp;
static PubSubClient mqttHa(haTcp);

static RelayController  relays;
static HaRelays         haRelays(relays);
static CompatWebServer  httpServer(80);

// -----------------------------------------------------------------------------
// Reconnect backoff (non-blocking, capped).
// -----------------------------------------------------------------------------
struct Reconnect {
    uint32_t nextAttemptMs = 0;
    uint32_t backoffMs     = 1000;
};
static Reconnect wifiReconnect, lanReconnect, haReconnect;
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
// MQTT dispatch
// -----------------------------------------------------------------------------
static void onHaMessage(char* topic, byte* payload, unsigned int length) {
    haRelays.handleMessage(topic, payload, length);
}

// Solar Assistant lives on the LAN broker only; the VPS bridge needs these
// six values for the Tuya virtual MUST device (and HA keeps their history).
// Forwarded on change, and at least once a minute.
static String   saLast[virtualMapSize];
static uint32_t saSentMs[virtualMapSize];

static void forwardSa(const String& topic, const String& value) {
    for (int i = 0; i < virtualMapSize; i++) {
        if (topic != virtualDeviceMap[i].mqttTopic) continue;
        if (!mqttHa.connected()) return;
        if (value == saLast[i] && millis() - saSentMs[i] < 60000) return;
        String t = String("dacha_relays/sa/") + virtualDeviceMap[i].tuyaCode;
        if (mqttHa.publish(t.c_str(), value.c_str(), false)) {
            saLast[i] = value;
            saSentMs[i] = millis();
        }
        return;
    }
}

static void onLanMessage(char* topic, byte* payload, unsigned int length) {
    String message;
    message.reserve(length);
    for (unsigned int i = 0; i < length; i++) message += (char)payload[i];
    String t(topic);
    relays.handleMqttMessage(t, message);
    forwardSa(t, message);
}

// -----------------------------------------------------------------------------
// WiFi: strongest of the configured networks.
// -----------------------------------------------------------------------------
struct WifiNetwork { const char* ssid; const char* password; };
static const WifiNetwork kWifiNetworks[] = {
    { WIFI_SSID, WIFI_PASSWORD },
#ifdef WIFI_SSID_2
    { WIFI_SSID_2, WIFI_PASSWORD_2 },
#endif
};
constexpr size_t   kWifiNetworkCount = sizeof(kWifiNetworks) / sizeof(kWifiNetworks[0]);
constexpr uint32_t kWifiAttemptMs = 10000;
constexpr uint32_t kWifiMaxRetryMs = 20000;
static size_t wifiNetworkIdx = 0;
static bool   wifiWasConnected = false;

static int scanForBestNetwork() {
    int n = WiFi.scanNetworks();
    int best = -1, bestRssi = -1000;
    String seen;
    for (int i = 0; i < n; i++) {
        for (size_t k = 0; k < kWifiNetworkCount; k++) {
            if (WiFi.SSID(i) == kWifiNetworks[k].ssid) {
                seen += " " + WiFi.SSID(i) + "=" + WiFi.RSSI(i);
                if (WiFi.RSSI(i) > bestRssi) { bestRssi = WiFi.RSSI(i); best = (int)k; }
            }
        }
    }
    WiFi.scanDelete();
    Log.printf("[WiFi] scan:%s\n", seen.isEmpty() ? " none of our networks heard" : seen.c_str());
    return best;
}

static void kickWifi() {
    uint32_t now = millis();
    if (WiFi.status() == WL_CONNECTED) {
        if (!wifiWasConnected) {
            wifiWasConnected = true;
            Log.printf("[WiFi] connected to %s, ip=%s, rssi=%d dBm\n",
                       kWifiNetworks[wifiNetworkIdx].ssid, WiFi.localIP().toString().c_str(), WiFi.RSSI());
        }
        resetBackoff(wifiReconnect);
        return;
    }
    if (wifiWasConnected) {
        wifiWasConnected = false;
        Log.println("[WiFi] connection lost");
    }
    if (now < wifiReconnect.nextAttemptMs) return;

    WiFi.mode(WIFI_STA);
    WiFi.hostname(kHostname);
    WiFi.setSleepMode(WIFI_NONE_SLEEP);   // mains-powered; sleep hurts a weak link
    WiFi.disconnect();
    int best = scanForBestNetwork();
    if (best >= 0) wifiNetworkIdx = best;
    else if (wifiReconnect.nextAttemptMs != 0) wifiNetworkIdx = (wifiNetworkIdx + 1) % kWifiNetworkCount;
    const WifiNetwork& net = kWifiNetworks[wifiNetworkIdx];
    Log.printf("[WiFi] connecting to %s (backoff=%u ms)\n", net.ssid, (unsigned)wifiReconnect.backoffMs);
    WiFi.begin(net.ssid, net.password);
    bumpBackoff(wifiReconnect);
    wifiReconnect.nextAttemptMs = now + constrain(wifiReconnect.backoffMs, kWifiAttemptMs, kWifiMaxRetryMs);
}

// -----------------------------------------------------------------------------
// Time
// -----------------------------------------------------------------------------
static bool timeReady = false;
static void kickTime() {
    if (timeReady) return;
    if (time(nullptr) > 1700000000) {
        timeReady = true;
        Log.println("[Time] synchronized");
    }
}

// -----------------------------------------------------------------------------
// MQTT
// -----------------------------------------------------------------------------
static bool kickLanMqtt() {
    if (WiFi.status() != WL_CONNECTED) return false;
    if (mqttLan.connected()) { resetBackoff(lanReconnect); return true; }
    if (millis() < lanReconnect.nextAttemptMs) return false;
    Log.printf("[MQTT LAN] connecting %s:%d (backoff=%u)\n", MQTT_LAN_HOST, MQTT_LAN_PORT,
               (unsigned)lanReconnect.backoffMs);
    mqttLan.setServer(MQTT_LAN_HOST, MQTT_LAN_PORT);
    mqttLan.setCallback(onLanMessage);
    mqttLan.setSocketTimeout(5);
    if (!mqttLan.connect("nodemcu-relays-lan", MQTT_LAN_USERNAME, MQTT_LAN_PASSWORD)) {
        Log.printf("[MQTT LAN] connect failed rc=%d\n", mqttLan.state());
        bumpBackoff(lanReconnect);
        return false;
    }
    Log.println("[MQTT LAN] connected");
    resetBackoff(lanReconnect);
    mqttLan.subscribe("solar_assistant/total/#");
    mqttLan.subscribe("solar_assistant/inverter_1/#");
#ifdef RELAY_TOPIC_PREFIX
    mqttLan.subscribe(RELAY_TOPIC_PREFIX "#");
#endif
    return true;
}

static bool kickHaMqtt() {
    if (WiFi.status() != WL_CONNECTED) return false;
    if (mqttHa.connected()) { resetBackoff(haReconnect); return true; }
    if (millis() < haReconnect.nextAttemptMs) return false;
    Log.printf("[MQTT HA] connecting %s:%d (backoff=%u)\n", MQTT_HA_HOST, MQTT_HA_PORT,
               (unsigned)haReconnect.backoffMs);
    mqttHa.setServer(MQTT_HA_HOST, MQTT_HA_PORT);
    mqttHa.setCallback(onHaMessage);
    mqttHa.setBufferSize(1536);   // discovery ~400 B, relay state ~1 KB
    mqttHa.setSocketTimeout(5);
    if (!mqttHa.connect("nodemcu-relays-ha", MQTT_HA_USERNAME, MQTT_HA_PASSWORD)) {
        Log.printf("[MQTT HA] connect failed rc=%d\n", mqttHa.state());
        bumpBackoff(haReconnect);
        return false;
    }
    Log.println("[MQTT HA] connected");
    resetBackoff(haReconnect);
    haRelays.onMqttConnected();
    return true;
}

// -----------------------------------------------------------------------------
// OTA (pio run -e nodemcu_ota -t upload)
// -----------------------------------------------------------------------------
static void beginArduinoOta() {
    ArduinoOTA.setHostname(kHostname);
    ArduinoOTA.setPassword(OTA_PASSWORD);
    ArduinoOTA.onStart([] { Log.println("[OTA] start"); });
    ArduinoOTA.onEnd([] { Log.println("[OTA] done, rebooting"); });
    ArduinoOTA.onError([](ota_error_t e) { Log.printf("[OTA] error %u\n", (unsigned)e); });
    ArduinoOTA.begin();
    Log.printf("[OTA] ready: %s.local\n", kHostname);
}

static void fillSystemStatus(JsonObject o) {
    o["build"] = String("NodeMCU ") + kFirmwareBuild;
    o["uptime_s"] = millis() / 1000;
    o["heap"] = ESP.getFreeHeap();
    o["heap_min"] = ESP.getMaxFreeBlockSize();   // largest free block: fragmentation
    o["reset_reason"] = compatResetName(compatResetKind());
    o["wifi_ssid"] = WiFi.SSID();
    o["wifi_rssi"] = WiFi.RSSI();
    o["ip"] = WiFi.localIP().toString();
    o["mqtt_lan"] = mqttLan.connected();
    o["mqtt_ha"] = mqttHa.connected();
    o["ha_tls"] = false;
    if (timeReady) {
        time_t now = time(nullptr);
        struct tm t;
        localtime_r(&now, &t);
        char buf[24];
        strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &t);
        o["local_time"] = buf;
    }
}

// -----------------------------------------------------------------------------
// Setup / loop
// -----------------------------------------------------------------------------
void setup() {
    setenv("TZ", kTimeZone, 1);
    tzset();
    Serial.begin(115200);
    Log.begin();
    delay(50);
    Log.printf("\n[Boot] NodeMCU dacha relays, build %s, reset: %s\n",
               kFirmwareBuild, compatResetName(compatResetKind()));

    String crash = compatLastCrash();
    if (crash.length()) Log.printf("[Crash] previous reset: %s\n", crash.c_str());

    // Relays first: restore their state before anything slow happens.
    relays.begin(settings::load());
    alerts.begin(compatResetKind());
    alerts.setCrashInfo(crash);

    haRelays.begin(&mqttHa, false);   // no TLS on this board: HA is read-only
    web_ui::begin(httpServer, relays, fillSystemStatus);

    configTime(kTimeZone, "pool.ntp.org", "time.google.com", "time.nist.gov");
    kickWifi();
}

void loop() {
    kickWifi();
    kickTime();

    bool lanOk = kickLanMqtt();
    bool haOk  = kickHaMqtt();
    if (lanOk) mqttLan.loop();
    if (haOk)  mqttHa.loop();

    relays.tick();
    alerts.tick(relays, mqttHa.connected());
    alerts.service();

    static bool netServicesStarted = false;
    if (WiFi.status() == WL_CONNECTED) {
        if (!netServicesStarted) {
            httpServer.begin();
            beginArduinoOta();
            netServicesStarted = true;
        }
        httpServer.handleClient();
        ArduinoOTA.handle();
    }

    if (haOk) haRelays.tick();
    delay(2);
}

#endif  // ESP8266
