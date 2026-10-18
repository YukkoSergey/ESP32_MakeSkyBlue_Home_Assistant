#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <WebServer.h>
#include <ArduinoOTA.h>
#include <esp_task_wdt.h>
#include <time.h>

#include "secrets.h"
#include "app_log.h"
#include "tuya_cloud_client.h"
#include "mppt_bridge.h"
#include "solar_assistant_bridge.h"
#include "solar_manager_protocol.h"
#include "settings.h"
#include "relay_controller.h"
#include "ha_relays.h"
#include "web_ui.h"
#include "alerts.h"

static const char* kFirmwareBuild = __DATE__ " " __TIME__;
static const char* kHostname = "dacha-esp32";

// -----------------------------------------------------------------------------
// Two MQTT brokers:
//   * LAN broker  — where Solar Assistant publishes solar_assistant/**
//                   (subscribe only; SolarAssistantBridge + relays)
//   * HA broker   — where Home Assistant + Solar_Manager listen for msbN/**
//                   (publish + subscribe; three SolarManagerDevice + relays)
//
// The HA broker connection is TLS when MQTT_HA_CA_CERT is defined in
// secrets.h; only then does the board accept commands from HA.
// -----------------------------------------------------------------------------
static WiFiClient   lanTcp;
static PubSubClient mqttLan(lanTcp);
#ifdef MQTT_HA_CA_CERT
static WiFiClientSecure haTcp;
static constexpr bool kHaTls = true;
#else
static WiFiClient   haTcp;
static constexpr bool kHaTls = false;
#endif
static PubSubClient mqttHa(haTcp);

static TuyaCloudClient tuyaClient(TUYA_API_BASE_URL, TUYA_CLIENT_ID, TUYA_CLIENT_SECRET);
static MpptBridge      mpptBridge(tuyaClient);
static SolarAssistantBridge saBridge(tuyaClient);

static RelayController relays;
static HaRelays        haRelays(relays);
static WebServer       httpServer(80);

// Loop watchdog: a hung loop reboots the board, and a reboot releases the
// relays. Tuya HTTPS runs in its own task, so the loop only blocks on MQTT
// connects.
constexpr uint32_t kLoopWdtTimeoutS = 60;

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
    if (haRelays.handleMessage(topic, payload, length)) return;
    for (auto& dev : smDevices) {
        if (dev.handleMessage(topic, payload, length)) return;
    }
}

static void onLanMessage(char* topic, byte* payload, unsigned int length) {
    String message;
    message.reserve(length);
    for (unsigned int i = 0; i < length; i++) message += (char)payload[i];
    String topicStr(topic);
    relays.handleMqttMessage(topicStr, message);
    saBridge.handleMqttMessage(topicStr, message);
}

// -----------------------------------------------------------------------------
// WiFi (non-blocking)
// -----------------------------------------------------------------------------
// Networks are tried in order; each failed attempt moves on to the next one.
struct WifiNetwork { const char* ssid; const char* password; };
static const WifiNetwork kWifiNetworks[] = {
    { WIFI_SSID, WIFI_PASSWORD },
#ifdef WIFI_SSID_2
    { WIFI_SSID_2, WIFI_PASSWORD_2 },
#endif
};
constexpr size_t   kWifiNetworkCount = sizeof(kWifiNetworks) / sizeof(kWifiNetworks[0]);
// WiFi.begin() needs several seconds to associate + get DHCP; don't abort
// an attempt (and switch network) before this.
constexpr uint32_t kWifiAttemptMs = 10000;
constexpr uint32_t kWifiMaxRetryMs = 20000;
static size_t wifiNetworkIdx = 0;
static bool   wifiWasConnected = false;

// Pick the configured network with the strongest signal. The board sits
// where one network is barely audible (-83 dBm in the shed), so blind
// rotation wasted half the attempts on the wrong one. Blocking, ~3 s.
static int scanForBestNetwork() {
    int n = WiFi.scanNetworks(false, false, false, 250);
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
            // Modem sleep makes a weak link drop and MQTT stall; we're mains-powered.
            WiFi.setSleep(false);
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
    WiFi.setHostname(kHostname);
    WiFi.disconnect();
    int best = scanForBestNetwork();
    if (best >= 0) {
        wifiNetworkIdx = best;
    } else if (wifiReconnect.nextAttemptMs != 0) {
        // Nothing heard: rotate blindly (hidden SSID, scan glitch).
        wifiNetworkIdx = (wifiNetworkIdx + 1) % kWifiNetworkCount;
    }
    const WifiNetwork& net = kWifiNetworks[wifiNetworkIdx];
    Log.printf("[WiFi] connecting to %s (backoff=%u ms)\n", net.ssid, (unsigned)wifiReconnect.backoffMs);
    WiFi.begin(net.ssid, net.password);
    bumpBackoff(wifiReconnect);
    // Keep retrying often: without WiFi the relays run blind on stale data.
    wifiReconnect.nextAttemptMs = now + constrain(wifiReconnect.backoffMs, kWifiAttemptMs, kWifiMaxRetryMs);
}

// -----------------------------------------------------------------------------
// Time (non-blocking check — Tuya signing needs sane epoch, seasons need the
// local date). Kyiv time zone with DST.
// -----------------------------------------------------------------------------
static const char* kTimeZone = "EET-2EEST,M3.5.0/3,M10.5.0/4";   // Kyiv
static volatile bool timeReady = false;
static void kickTime() {
    if (timeReady) return;
    time_t now = time(nullptr);
    if (now > 1700000000) {   // 2023-11-14
        timeReady = true;
        Log.println("[Time] synchronized");
    }
}
static void beginNtp() {
    configTzTime(kTimeZone, "pool.ntp.org", "time.google.com", "time.nist.gov");
}

// -----------------------------------------------------------------------------
// MQTT connect helpers
// -----------------------------------------------------------------------------
static bool kickLanMqtt() {
    if (WiFi.status() != WL_CONNECTED) return false;
    if (mqttLan.connected()) { resetBackoff(lanReconnect); return true; }
    if (millis() < lanReconnect.nextAttemptMs) return false;

    Log.printf("[MQTT LAN] connecting %s:%d (backoff=%u)\n",
                  MQTT_LAN_HOST, MQTT_LAN_PORT, (unsigned)lanReconnect.backoffMs);
    mqttLan.setServer(MQTT_LAN_HOST, MQTT_LAN_PORT);
    mqttLan.setCallback(onLanMessage);

    bool ok = mqttLan.connect("esp32-lan", MQTT_LAN_USERNAME, MQTT_LAN_PASSWORD);
    if (!ok) {
        Log.printf("[MQTT LAN] connect failed rc=%d\n", mqttLan.state());
        bumpBackoff(lanReconnect);
        return false;
    }
    Log.println("[MQTT LAN] connected");
    resetBackoff(lanReconnect);
    mqttLan.subscribe("solar_assistant/total/#");
    mqttLan.subscribe("solar_assistant/inverter_1/#");
#ifdef RELAY_TOPIC_PREFIX
    mqttLan.subscribe(RELAY_TOPIC_PREFIX "#");   // bench testing, see relay_controller.cpp
#endif
    return true;
}

static bool kickHaMqtt() {
    if (WiFi.status() != WL_CONNECTED) return false;
    if (mqttHa.connected()) { resetBackoff(haReconnect); return true; }
    if (millis() < haReconnect.nextAttemptMs) return false;

    Log.printf("[MQTT HA] connecting %s:%d%s (backoff=%u)\n",
                  MQTT_HA_HOST, MQTT_HA_PORT, kHaTls ? " TLS" : "", (unsigned)haReconnect.backoffMs);
    mqttHa.setServer(MQTT_HA_HOST, MQTT_HA_PORT);
    mqttHa.setCallback(onHaMessage);
    // HA discovery configs are ~400 bytes, the relay state JSON ~1 KB.
    mqttHa.setBufferSize(2048);

    // Global LWT: single topic can't cover 3 devices, so we use the first as a
    // health marker and each device republishes 'online' on connect. To publish
    // 'offline' per device on ungraceful drop we'd need one MQTT client per
    // device — deferred; a graceful shutdown path (below) covers restarts.
    bool ok = mqttHa.connect(
        "esp32-ha", MQTT_HA_USERNAME, MQTT_HA_PASSWORD,
        (String(MSB_SERIAL_1) + "/online").c_str(), 1, false, "offline"
    );
    if (!ok) {
        Log.printf("[MQTT HA] connect failed rc=%d\n", mqttHa.state());
        bumpBackoff(haReconnect);
        return false;
    }
    Log.println("[MQTT HA] connected");
    resetBackoff(haReconnect);
    for (auto& dev : smDevices) {
        dev.setMqttClient(&mqttHa);
        dev.onMqttConnected();
    }
    haRelays.onMqttConnected();
    return true;
}

// -----------------------------------------------------------------------------
// Tuya task. Every Tuya call is a fresh HTTPS handshake (seconds each); doing
// that in loop() starved MQTT keepalives and dropped both brokers. Here it can
// block as long as it likes — relays, MQTT and the web UI keep running.
// -----------------------------------------------------------------------------
static volatile uint32_t tuyaLastOkMs = 0;

static void tuyaTask(void*) {
    uint32_t lastMpptPoll = 0, lastSaSync = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(200));
        if (!timeReady || WiFi.status() != WL_CONNECTED) continue;   // signing needs epoch

        if (millis() - lastMpptPoll > 10000) {
            lastMpptPoll = millis();
            if (mpptBridge.updateAll() > 0) tuyaLastOkMs = millis();
        }
        if (millis() - lastSaSync > 10000) {
            lastSaSync = millis();
            saBridge.processPendingCommands();
        }
    }
}

// -----------------------------------------------------------------------------
// OTA from the dev machine: pio run -e ota -t upload
// -----------------------------------------------------------------------------
static void beginArduinoOta() {
    ArduinoOTA.setHostname(kHostname);
    ArduinoOTA.setPassword(OTA_PASSWORD);
    ArduinoOTA.onStart([] { Log.println("[OTA] start"); });
    ArduinoOTA.onProgress([](unsigned int, unsigned int) { esp_task_wdt_reset(); });
    ArduinoOTA.onEnd([] { Log.println("[OTA] done, rebooting"); });
    ArduinoOTA.onError([](ota_error_t e) { Log.printf("[OTA] error %u\n", (unsigned)e); });
    ArduinoOTA.begin();
    Log.printf("[OTA] ready: %s.local\n", kHostname);
}

static const char* resetReasonName(esp_reset_reason_t r) {
    switch (r) {
        case ESP_RST_POWERON:   return "power-on";
        case ESP_RST_SW:        return "software restart";
        case ESP_RST_PANIC:     return "crash (panic)";
        case ESP_RST_INT_WDT:   return "interrupt watchdog";
        case ESP_RST_TASK_WDT:  return "task watchdog";
        case ESP_RST_WDT:       return "watchdog";
        case ESP_RST_BROWNOUT:  return "brownout (power dip)";
        case ESP_RST_DEEPSLEEP: return "deep sleep";
        case ESP_RST_EXT:       return "external reset";
        default:                return "other";
    }
}

static void fillSystemStatus(JsonObject o) {
    o["build"] = kFirmwareBuild;
    o["uptime_s"] = millis() / 1000;
    o["heap"] = ESP.getFreeHeap();
    o["heap_min"] = ESP.getMinFreeHeap();
    o["reset_reason"] = resetReasonName(esp_reset_reason());
    o["wifi_ssid"] = WiFi.SSID();
    o["wifi_rssi"] = WiFi.RSSI();
    o["ip"] = WiFi.localIP().toString();
    o["mqtt_lan"] = mqttLan.connected();
    o["mqtt_ha"] = mqttHa.connected();
    o["ha_tls"] = kHaTls;
    o["tuya_last_ok_s"] = tuyaLastOkMs ? (long)((millis() - tuyaLastOkMs) / 1000) : -1L;
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
    // The RTC keeps running across a soft reboot; set the zone first so even
    // the earliest log lines and the season are in local time.
    setenv("TZ", kTimeZone, 1);
    tzset();
    Serial.begin(115200);
    Log.begin();
    delay(50);
    Log.printf("\n[Boot] ESP32 dacha bridge, build %s, reset: %s\n",
               kFirmwareBuild, resetReasonName(esp_reset_reason()));

    // Relays first: restore their state before anything slow happens.
    relays.begin(settings::load());
    alerts.begin(compatResetKind());

    esp_task_wdt_init(kLoopWdtTimeoutS, true);
    esp_task_wdt_add(NULL);

#ifdef MQTT_HA_CA_CERT
    haTcp.setCACert(MQTT_HA_CA_CERT);
#endif
    haRelays.begin(&mqttHa, kHaTls);
    web_ui::begin(httpServer, relays, fillSystemStatus);

    kickWifi();
    beginNtp();

    // Same core as loop() (1), so it never competes with the WiFi stack on 0.
    xTaskCreatePinnedToCore(tuyaTask, "tuya", 16384, nullptr, 1, nullptr, 1);
}

void loop() {
    esp_task_wdt_reset();

    kickWifi();
    kickTime();

    bool lanOk = kickLanMqtt();
    bool haOk  = kickHaMqtt();

    if (lanOk) mqttLan.loop();
    if (haOk)  mqttHa.loop();

    relays.tick();
    alerts.tick(relays, mqttHa.connected());

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

    if (haOk) {
        haRelays.tick();
        if (timeReady) {
            for (int i = 0; i < 3; i++) {
                smDevices[i].tick(mpptBridge.getState(i));
            }
        }
    }

    delay(2);   // let the idle task run
}
