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

// Canary zones around each WiFiClient: the crash trace showed the HA
// client's object (its vtable pointer) overwritten by something that looked
// like MQTT packet bytes. checkGuards() catches the overwrite every loop pass
// and records the intruding bytes before anything calls into the object.
struct GuardedClient {
    static constexpr uint32_t kCanary = 0xA5C3E1F0;
    uint32_t   pre[12];
    WiFiClient c;
    uint32_t   post[12];
    uint32_t   vtbl;   // the object's vtable pointer as constructed
    GuardedClient() {
        for (auto& w : pre) w = kCanary;
        for (auto& w : post) w = kCanary;
        vtbl = *(const uint32_t*)&c;
    }
    int damaged() const {
        for (auto w : pre)  if (w != kCanary) return 1;
        for (auto w : post) if (w != kCanary) return 2;
        if (*(const uint32_t*)&c != vtbl) return 3;   // object itself hit
        return 0;
    }
};

// A fresh client for every connection attempt (see freshClient()).
static GuardedClient* lanTcp = nullptr;
static PubSubClient   mqttLan;
static GuardedClient* haTcp = nullptr;
static PubSubClient   mqttHa;

static void checkGuards() {
    GuardedClient* gs[2] = {lanTcp, haTcp};
    for (int i = 0; i < 2; i++) {
        if (!gs[i]) continue;
        int where = gs[i]->damaged();
        if (!where) continue;
        // zone: 1x = LAN client, 2x = HA client; x=1 before the object, 2 after
        const uint8_t* bytes = where == 1 ? (const uint8_t*)gs[i]->pre
                             : where == 2 ? (const uint8_t*)gs[i]->post
                                          : (const uint8_t*)&gs[i]->c;
        compatGuardTripped((i + 1) * 10 + where, bytes, 48);
    }
}

// Never reuse a WiFiClient after its connection died. On ESP8266 a
// ClientContext left from a broken link (seen on the internet link to the HA
// broker, WiFi itself fine) keeps a dangling lwIP pcb, and the next
// PubSubClient::connected() crashed in ClientContext::state(). Throwing the
// object away and making a new one leaves nothing dangling to touch.
static void freshClient(GuardedClient*& c, PubSubClient& mqtt) {
    if (c) {
        c->c.stop();
        delete c;
    }
    c = new GuardedClient();
    // Short write timeout: on a slow internet link a publish waiting 5 s for
    // TCP acks (the default) stalled the whole loop — the web page went dark
    // for minutes before each crash.
    c->c.setTimeout(1500);
    mqtt.setClient(c->c);
}

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
// Forwarded on change, and at least once a minute. Fixed buffers: this runs
// for every SA message and must not touch the heap.
static char     saLast[virtualMapSize][16];
static uint32_t saSentMs[virtualMapSize];

static void forwardSa(const char* topic, const char* value) {
    for (int i = 0; i < virtualMapSize; i++) {
        if (strcmp(topic, virtualDeviceMap[i].mqttTopic) != 0) continue;
        if (!mqttHa.connected()) return;
        // Changed values at most every 15 s, unchanged ones every 2 min: the
        // Tuya virtual device and HA history don't need more, and the
        // internet link to the HA broker is the board's weakest spot.
        uint32_t age = millis() - saSentMs[i];
        bool changed = strcmp(value, saLast[i]) != 0;
        if (saSentMs[i] && (changed ? age < 15000 : age < 120000)) return;
        char t[48];
        snprintf(t, sizeof(t), "dacha_relays/sa/%s", virtualDeviceMap[i].tuyaCode);
        if (mqttHa.publish(t, value, false)) {
            strncpy(saLast[i], value, sizeof(saLast[i]) - 1);
            saLast[i][sizeof(saLast[i]) - 1] = 0;
            saSentMs[i] = millis();
        }
        return;
    }
}

static void onLanMessage(char* topic, byte* payload, unsigned int length) {
    char value[32];   // SA payloads are short numbers / words
    unsigned int n = length < sizeof(value) - 1 ? length : sizeof(value) - 1;
    memcpy(value, payload, n);
    value[n] = 0;
    relays.handleMqttMessage(topic, value);
    forwardSa(topic, value);
}

// Exact topics instead of solar_assistant/total/# and inverter_1/#: SA
// publishes dozens of values every few seconds and we need eight.
static void subscribeLan() {
    for (size_t i = 0; i < RelayController::inputTopicCount(); i++)
        mqttLan.subscribe(RelayController::inputTopic(i));
    for (int i = 0; i < virtualMapSize; i++) {
        bool dup = false;
        for (size_t k = 0; k < RelayController::inputTopicCount(); k++)
            if (strcmp(virtualDeviceMap[i].mqttTopic, RelayController::inputTopic(k)) == 0) dup = true;
        if (!dup) mqttLan.subscribe(virtualDeviceMap[i].mqttTopic);
    }
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

// Drop both MQTT clients and their TCP connections. Called the moment WiFi is
// lost, before any WiFi re-init: a ClientContext left over from the dead link
// ended up with a dangling lwIP pcb, and the next connected()/loop() on it
// crashed (ClientContext::state, exccause 9, garbage address).
static void stopNetClients() {
    mqttLan.disconnect();
    mqttHa.disconnect();
    if (lanTcp) lanTcp->c.stop();
    if (haTcp) haTcp->c.stop();
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
        g_wifiDrops++;
        stopNetClients();
        Log.println("[WiFi] connection lost, MQTT clients closed");
    }
    if (now < wifiReconnect.nextAttemptMs) return;

    static bool wifiInit = false;
    if (!wifiInit) {   // once: these re-init the interface
        WiFi.persistent(false);
        WiFi.mode(WIFI_STA);
        WiFi.hostname(kHostname);
        WiFi.setSleepMode(WIFI_NONE_SLEEP);   // mains-powered; sleep hurts a weak link
        wifiInit = true;
    }
    WiFi.disconnect(false);
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
    freshClient(lanTcp, mqttLan);
    mqttLan.setServer(MQTT_LAN_HOST, MQTT_LAN_PORT);
    mqttLan.setCallback(onLanMessage);
    mqttLan.setSocketTimeout(2);
    mqttLan.setKeepAlive(60);   // weak WiFi in the shed: don't drop on a few lost packets
    if (!mqttLan.connect("nodemcu-relays-lan", MQTT_LAN_USERNAME, MQTT_LAN_PASSWORD)) {
        Log.printf("[MQTT LAN] connect failed rc=%d\n", mqttLan.state());
        bumpBackoff(lanReconnect);
        return false;
    }
    Log.println("[MQTT LAN] connected");
    resetBackoff(lanReconnect);
    subscribeLan();
    return true;
}

static bool kickHaMqtt() {
    if (WiFi.status() != WL_CONNECTED) return false;
    if (mqttHa.connected()) { resetBackoff(haReconnect); return true; }
    if (millis() < haReconnect.nextAttemptMs) return false;
    Log.printf("[MQTT HA] connecting %s:%d (backoff=%u)\n", MQTT_HA_HOST, MQTT_HA_PORT,
               (unsigned)haReconnect.backoffMs);
    freshClient(haTcp, mqttHa);
    mqttHa.setServer(MQTT_HA_HOST, MQTT_HA_PORT);
    mqttHa.setCallback(onHaMessage);
    mqttHa.setBufferSize(1536);   // discovery ~400 B, relay state ~1 KB
    mqttHa.setSocketTimeout(2);
    mqttHa.setKeepAlive(60);
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
    checkGuards();
    g_crumb = 1; kickWifi();
    g_crumb = 2; kickTime();

    bool wifiUp = WiFi.status() == WL_CONNECTED;
    g_crumb = 3; bool lanOk = wifiUp && kickLanMqtt();
    g_crumb = 4; bool haOk  = wifiUp && kickHaMqtt();
    g_crumb = 5; if (lanOk) mqttLan.loop();
    checkGuards();
    g_crumb = 6; if (haOk)  mqttHa.loop();
    checkGuards();

    g_crumb = 7; relays.tick();
    // haOk already says whether the HA link is up; every extra connected()
    // is another touch of the TCP context, so don't ask again this pass.
    g_crumb = 8; alerts.tick(relays, haOk);

    static bool netServicesStarted = false;
    if (wifiUp) {
        if (!netServicesStarted) {
            httpServer.begin();
            beginArduinoOta();
            netServicesStarted = true;
        }
        g_crumb = 9;  httpServer.handleClient();
        g_crumb = 10; ArduinoOTA.handle();
    }

    g_crumb = 11; if (haOk) haRelays.tick();
    checkGuards();
    g_crumb = 0;
    // 10 ms is plenty for relays, MQTT and the web page, and cuts the number
    // of passes (and TCP-context touches) fivefold compared with 2 ms.
    delay(10);
}

#endif  // ESP8266
