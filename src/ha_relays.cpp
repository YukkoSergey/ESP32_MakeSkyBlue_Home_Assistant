#include "ha_relays.h"
#include "app_log.h"
#include "settings.h"
#include "alerts.h"
#include <ArduinoJson.h>

static const char* kBase             = "dacha_relays";
static const char* kStateTopic       = "dacha_relays/state";
static const char* kGenModeSetTopic  = "dacha_relays/gen_mode/set";
static const char* kSeasonSetTopic   = "dacha_relays/season_mode/set";
static const char* kHeavyModeSetTopic = "dacha_relays/heavy_mode/set";
static const char* kDiscoveryPrefix  = "homeassistant";
static const char* kAlertTopic       = "dacha_relays/alert";
static const char* kAlertAckTopic    = "dacha_relays/alert/ack";
static const char* kHeartbeatTopic   = "dacha_relays/ha_heartbeat";
static const char* kHaStatusTopic    = "homeassistant/status";   // HA birth / LWT
// Signed commands from the VPS bridge (dacha-bridge): {"c","a","t","s"} with
// s = HMAC-SHA256(CMD_SECRET, "c|a|t"). Works without TLS: a sniffed broker
// password isn't enough to forge or replay one.
static const char* kSignedCmdTopic   = "dacha_relays/cmd";
static constexpr long kCmdMaxSkewS   = 300;

static constexpr uint32_t kPublishIntervalMs = 30000;
static constexpr uint32_t kMinPublishGapMs   = 1000;
// HA marks sensors unavailable if the board goes quiet for this long.
static constexpr int      kExpireAfterS      = 120;

static const char* kDeviceJson =
    "\"dev\":{\"ids\":[\"esp32_dacha_relays\"],\"name\":\"Dacha relays\","
    "\"mdl\":\"ESP32 bridge\",\"mf\":\"DIY\"}";

void HaRelays::begin(PubSubClient* mqtt, bool commandsEnabled) {
    _mqtt = mqtt;
    _commandsEnabled = commandsEnabled;
}

void HaRelays::publishEntity(const char* component, const char* id, const char* name,
                             const char* valueTemplate, const char* extraJson) {
    String topic = String(kDiscoveryPrefix) + "/" + component + "/" + kBase + "/" + id + "/config";
    String p = "{";
    p += "\"name\":\"" + String(name) + "\",";
    p += "\"uniq_id\":\"" + String(kBase) + "_" + id + "\",";
    p += "\"stat_t\":\"" + String(kStateTopic) + "\",";
    p += "\"val_tpl\":\"" + String(valueTemplate) + "\",";
    if (extraJson && *extraJson) p += String(extraJson) + ",";
    p += kDeviceJson;
    p += "}";
    if (!_mqtt->publish(topic.c_str(), p.c_str(), true)) {
        Log.printf("[HA relays] discovery publish failed for %s (%u bytes)\n", id, p.length());
    }
}

static void clearEntity(PubSubClient* mqtt, const char* component, const char* id) {
    String topic = String(kDiscoveryPrefix) + "/" + component + "/" + kBase + "/" + id + "/config";
    mqtt->publish(topic.c_str(), "", true);
}

void HaRelays::publishDiscovery() {
    char exp[32];
    snprintf(exp, sizeof(exp), "\"exp_aft\":%d", kExpireAfterS);
    String e(exp);

    publishEntity("sensor", "soc", "Battery SOC", "{{ value_json.soc }}",
                  (e + ",\"unit_of_meas\":\"%\",\"dev_cla\":\"battery\"").c_str());
    publishEntity("sensor", "reserve", "Battery reserve", "{{ value_json.reserve_kwh }}",
                  (e + ",\"unit_of_meas\":\"kWh\",\"dev_cla\":\"energy_storage\"").c_str());
    publishEntity("sensor", "season", "Active season", "{{ value_json.season }}", e.c_str());
    publishEntity("sensor", "battery_w", "Battery power", "{{ value_json.battery_w }}",
                  (e + ",\"unit_of_meas\":\"W\",\"dev_cla\":\"power\"").c_str());
    publishEntity("sensor", "gen_session", "Generator session energy", "{{ value_json.gen_session_kwh }}",
                  (e + ",\"unit_of_meas\":\"kWh\",\"dev_cla\":\"energy\"").c_str());
    publishEntity("sensor", "gen_eta", "Generator time to goal",
                  "{{ value_json.gen_eta_h if value_json.gen_eta_h >= 0 else None }}",
                  (e + ",\"unit_of_meas\":\"h\",\"dev_cla\":\"duration\"").c_str());

    publishEntity("binary_sensor", "heavy_load", "Heavy load allowed",
                  "{{ 'ON' if value_json.heavy_load else 'OFF' }}", (e + ",\"dev_cla\":\"power\"").c_str());
    publishEntity("binary_sensor", "generator", "Generator relay",
                  "{{ 'ON' if value_json.generator else 'OFF' }}", (e + ",\"dev_cla\":\"running\"").c_str());
    publishEntity("binary_sensor", "gen_fb", "Generator running (feedback)",
                  "{{ 'ON' if value_json.gen_fb_running else 'OFF' }}", (e + ",\"dev_cla\":\"running\"").c_str());
    publishEntity("binary_sensor", "gen_no_start", "Generator did not start",
                  "{{ 'ON' if value_json.gen_no_start else 'OFF' }}", (e + ",\"dev_cla\":\"problem\"").c_str());
    publishEntity("binary_sensor", "soc_frozen", "Battery SOC frozen",
                  "{{ 'ON' if value_json.soc_frozen else 'OFF' }}", (e + ",\"dev_cla\":\"problem\"").c_str());
    publishEntity("binary_sensor", "volt_emergency", "Battery voltage emergency",
                  "{{ 'ON' if value_json.volt_emergency else 'OFF' }}", (e + ",\"dev_cla\":\"problem\"").c_str());
    publishEntity("sensor", "inv_battery_v", "Battery voltage (inverter)", "{{ value_json.inv_battery_v }}",
                  (e + ",\"unit_of_meas\":\"V\",\"dev_cla\":\"voltage\"").c_str());
    publishEntity("binary_sensor", "soc_fresh", "Solar Assistant data",
                  "{{ 'ON' if value_json.soc_fresh else 'OFF' }}", (e + ",\"dev_cla\":\"connectivity\"").c_str());
    // Removed entities: the generator feeds a LiFePO4 charger, not the
    // inverter, so the inverter AC input says nothing about it. Clearing the
    // retained configs makes HA drop them.
    clearEntity(_mqtt, "sensor", "ac_input");
    clearEntity(_mqtt, "binary_sensor", "gen_no_ac");

    if (_commandsEnabled) {
        clearEntity(_mqtt, "sensor", "gen_mode");
        clearEntity(_mqtt, "sensor", "season_mode");
        clearEntity(_mqtt, "sensor", "heavy_mode");
        publishEntity("select", "heavy_mode", "Heavy load mode", "{{ value_json.heavy_mode }}",
                      (String("\"cmd_t\":\"") + kHeavyModeSetTopic + "\",\"options\":[\"auto\",\"on\",\"off\"]").c_str());
        publishEntity("select", "gen_mode", "Generator mode", "{{ value_json.gen_mode }}",
                      (String("\"cmd_t\":\"") + kGenModeSetTopic + "\",\"options\":[\"auto\",\"run\",\"off\"]").c_str());
        publishEntity("select", "season_mode", "Season mode", "{{ value_json.season_mode }}",
                      (String("\"cmd_t\":\"") + kSeasonSetTopic +
                       "\",\"options\":[\"auto\",\"summer\",\"shoulder\",\"winter\"]").c_str());
    } else {
        clearEntity(_mqtt, "select", "gen_mode");
        clearEntity(_mqtt, "select", "season_mode");
        clearEntity(_mqtt, "select", "heavy_mode");
        publishEntity("sensor", "heavy_mode", "Heavy load mode", "{{ value_json.heavy_mode }}", e.c_str());
        publishEntity("sensor", "gen_mode", "Generator mode", "{{ value_json.gen_mode }}", e.c_str());
        publishEntity("sensor", "season_mode", "Season mode", "{{ value_json.season_mode }}", e.c_str());
    }
}

void HaRelays::onMqttConnected() {
    if (!_mqtt) return;
    publishDiscovery();
    // Read-only health signals: safe without TLS.
    _mqtt->subscribe(kHeartbeatTopic);
    _mqtt->subscribe(kHaStatusTopic);
#ifdef CMD_SECRET
    _mqtt->subscribe(kSignedCmdTopic);
#endif
    if (_commandsEnabled) {
        _mqtt->subscribe(kGenModeSetTopic);
        _mqtt->subscribe(kSeasonSetTopic);
        _mqtt->subscribe(kHeavyModeSetTopic);
        _mqtt->subscribe(kAlertAckTopic);
    }
    Log.printf("[HA relays] discovery published, commands %s\n",
               _commandsEnabled ? "enabled (TLS)" : "disabled (no TLS)");
    publishState();
}

void HaRelays::publishState() {
    // Heap, not stack: the ESP8266 stack is only 4 KB.
    DynamicJsonDocument doc(1536);
    _relays.fillStatus(doc.to<JsonObject>());
    String buf;
    serializeJson(doc, buf);
    _mqtt->publish(kStateTopic, (const uint8_t*)buf.c_str(), buf.length(), false);
    _lastPublishMs = millis();
    _lastChange = _relays.changeCounter();
}

bool HaRelays::handleMessage(const char* topic, const uint8_t* payload, unsigned int length) {
    if (strcmp(topic, kHeartbeatTopic) == 0) { alerts.onHaHeartbeat(); return true; }
    if (strcmp(topic, kSignedCmdTopic) == 0) { handleSignedCommand(payload, length); return true; }
    if (strcmp(topic, kHaStatusTopic) == 0) {
        alerts.onHaStatus(length >= 6 && memcmp(payload, "online", 6) == 0);
        return true;
    }
    if (strcmp(topic, kAlertAckTopic) == 0) {
        // Acks only over TLS too: silencing alarms is a command.
        if (_commandsEnabled) {
            String k;
            for (unsigned i = 0; i < length; i++) k += (char)payload[i];
            k.trim();
            alerts.ack(k, "HA");
        }
        return true;
    }
    bool gen = strcmp(topic, kGenModeSetTopic) == 0;
    bool season = strcmp(topic, kSeasonSetTopic) == 0;
    bool heavy = strcmp(topic, kHeavyModeSetTopic) == 0;
    if (!gen && !season && !heavy) return false;
    if (!_commandsEnabled) return true;   // never act without TLS

    String v;
    for (unsigned i = 0; i < length; i++) v += (char)payload[i];
    v.trim();

    if (heavy) {
        HeavyMode m;
        if (RelayController::parseHeavyMode(v, m)) _relays.setHeavyMode(m, "HA");
    } else if (gen) {
        GenMode m;
        if (RelayController::parseGenMode(v, m)) _relays.setGenMode(m, "HA");
        else Log.printf("[HA relays] bad generator mode '%s'\n", v.c_str());
    } else {
        static const char* names[] = {"auto", "summer", "shoulder", "winter"};
        for (uint8_t i = 0; i < 4; i++) {
            if (v == names[i]) {
                Settings s = _relays.currentSettings();
                s.seasonMode = (SeasonMode)i;
                settings::save(s);
                _relays.applySettings(s);
                Log.printf("[HA relays] season mode -> %s (by HA)\n", names[i]);
                break;
            }
        }
    }
    publishState();
    return true;
}

void HaRelays::handleSignedCommand(const uint8_t* payload, unsigned int length) {
#ifdef CMD_SECRET
    DynamicJsonDocument d(512);
    if (deserializeJson(d, payload, length)) return;
    String c = d["c"] | "", a = d["a"] | "", s = d["s"] | "";
    long t = d["t"] | 0L;
    long now = (long)time(nullptr);
    if (now < 1700000000 || labs(now - t) > kCmdMaxSkewS || t <= _lastCmdTs) {
        Log.printf("[CMD] rejected '%s' (stale or replayed)\n", c.c_str());
        return;
    }
    if (compatHmacSha256Hex(CMD_SECRET, c + "|" + a + "|" + String(t)) != s) {
        Log.printf("[CMD] rejected '%s' (bad signature)\n", c.c_str());
        return;
    }
    _lastCmdTs = t;
    Log.printf("[CMD] signed command: %s %s\n", c.c_str(), a.c_str());
    if (c == "ack") {
        alerts.ack(a, "Telegram via VPS");
    } else if (c == "heavy_mode") {
        HeavyMode m;
        if (RelayController::parseHeavyMode(a, m)) _relays.setHeavyMode(m, "VPS");
    } else if (c == "gen_mode") {
        GenMode m;
        if (RelayController::parseGenMode(a, m)) _relays.setGenMode(m, "VPS");
    }
    publishState();
#else
    (void)payload; (void)length;
#endif
}

void HaRelays::tick() {
    if (!_mqtt || !_mqtt->connected()) return;
    String alert;
    while (alerts.peekHaMessage(alert)) {
        if (!_mqtt->publish(kAlertTopic, alert.c_str(), false)) {
            Log.println("[HA relays] alert publish failed, will retry");
            break;
        }
        alerts.dropHaMessage();
    }
    uint32_t now = millis();
    bool changed = _relays.changeCounter() != _lastChange;
    if ((changed && now - _lastPublishMs > kMinPublishGapMs) || now - _lastPublishMs > kPublishIntervalMs) {
        publishState();
    }
}
