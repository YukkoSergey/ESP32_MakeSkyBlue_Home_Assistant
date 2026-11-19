#include "relay_controller.h"
#include "app_log.h"
#include <time.h>
#include <math.h>

// Bench testing: -DRELAY_TOPIC_PREFIX=\"bench/\" moves every relay input to
// bench/solar_assistant/..., so fake values neither mix with the real SA
// stream nor reach the old ESP8266.
#ifndef RELAY_TOPIC_PREFIX
#define RELAY_TOPIC_PREFIX ""
#endif
static const char* kSocTopic          = RELAY_TOPIC_PREFIX "solar_assistant/total/battery_state_of_charge/state";
static const char* kBatteryPowerTopic = RELAY_TOPIC_PREFIX "solar_assistant/total/battery_power/state";
// Measured by the inverter itself, independent of the SA <-> BMS link.
static const char* kLoadTopic         = RELAY_TOPIC_PREFIX "solar_assistant/inverter_1/load_power/state";
static const char* kGridVTopic        = RELAY_TOPIC_PREFIX "solar_assistant/inverter_1/grid_voltage/state";
static const char* kInvBatVTopic      = RELAY_TOPIC_PREFIX "solar_assistant/inverter_1/battery_voltage/state";
static const char* kInvBatITopic      = RELAY_TOPIC_PREFIX "solar_assistant/inverter_1/battery_current/state";
// Cherokee chargers. SA's LAN MQTT (checked 2026-11-19) has no
// ac_input_voltage_range, total/battery_voltage, total cell maximum or CCL —
// those exist only in its cloud integration (HA). Subscribed anyway in case a
// later SA version publishes them; until then the cell maximum and BMS
// voltage come from the per-pack topics, the input range from the HA hint,
// and the CCL rule simply has no data.
static const char* kPvTopic           = RELAY_TOPIC_PREFIX "solar_assistant/inverter_1/pv_power/state";
static const char* kDevModeTopic      = RELAY_TOPIC_PREFIX "solar_assistant/inverter_1/device_mode/state";
static const char* kRangeTopic        = RELAY_TOPIC_PREFIX "solar_assistant/inverter_1/ac_input_voltage_range/state";
static const char* kOutPrioTopic      = RELAY_TOPIC_PREFIX "solar_assistant/inverter_1/output_source_priority/state";
static const char* kChgPrioTopic      = RELAY_TOPIC_PREFIX "solar_assistant/inverter_1/charger_source_priority/state";
static const char* kTotBmsVTopic      = RELAY_TOPIC_PREFIX "solar_assistant/total/battery_voltage/state";
static const char* kTotCellTopic      = RELAY_TOPIC_PREFIX "solar_assistant/total/battery_cell_voltage_-_highest/state";
static const char* kCclTopic          = RELAY_TOPIC_PREFIX "solar_assistant/total/battery_recommended_charge_current/state";
static const char* kBatTempTopic      = RELAY_TOPIC_PREFIX "solar_assistant/total/battery_temperature/state";
#define PACK_TOPICS(n) RELAY_TOPIC_PREFIX "solar_assistant/battery_" #n "/cell_voltage_-_highest/state", \
                       RELAY_TOPIC_PREFIX "solar_assistant/battery_" #n "/voltage/state"
static const char* const kPackTopics[8] = { PACK_TOPICS(1), PACK_TOPICS(2), PACK_TOPICS(3), PACK_TOPICS(4) };

// Same freshness rule for the auxiliary readings as for SOC.
static constexpr uint32_t kAuxStaleMs = RelayController::kSocStaleMs;
// Inverter settings (priorities, mode words) come every ~5 min.
static constexpr uint32_t kSettingStaleMs = 15UL * 60UL * 1000UL;
static constexpr uint32_t kHintTtlMs = 3UL * 60UL * 60UL * 1000UL;

// Relay polarity is a build setting (RELAY_ACTIVE_HIGH), never a runtime
// one: a wrong polarity closes the generator and heavy-load contacts exactly
// when the board means to open them.
//
// * Active-HIGH (env:nodemcu): red "2 Relay Module high/low level trigger"
//   with S1/S2 on High. LOW or a floating input = relay OFF, so boot, reset
//   and a broken wire all release the relays; no 3.3 V vs 5 V issue.
// * Active-LOW (env:esp32dev): the old blue JD-VCC module. On an ESP32 the
//   pins are open-drain and the module needs its inputs on 3.3 V (JD-VCC
//   jumper off) — with 5 V on its inputs the relays never released.
#ifdef RELAY_ACTIVE_HIGH
static constexpr bool kActiveHigh = true;
#else
static constexpr bool kActiveHigh = false;
#endif
static constexpr uint8_t kRelayOffLevel = kActiveHigh ? LOW : HIGH;

static void writeRelay(uint8_t pin, bool on) {
    digitalWrite(pin, on == kActiveHigh ? HIGH : LOW);
}

// Strict number parse: "unavailable" and friends must not become 0.
// Works on the raw payload — no String, no heap: this runs for every SA
// message, and heap churn is what the ESP8266 can least afford.
static bool parseNumber(const char* p, float& out) {
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    const char* start = p;
    bool seenDigit = false, seenDot = false;
    for (; *p && *p != ' ' && *p != '\r' && *p != '\n'; p++) {
        char c = *p;
        if (p == start && (c == '-' || c == '+')) continue;
        if (c == '.' && !seenDot) { seenDot = true; continue; }
        if (!isdigit((unsigned char)c)) return false;
        seenDigit = true;
    }
    if (!seenDigit) return false;
    out = (float)atof(start);
    return true;
}

static const char* const kInputTopics[] = {
    kSocTopic, kBatteryPowerTopic, kLoadTopic, kGridVTopic, kInvBatVTopic, kInvBatITopic,
    kPvTopic, kDevModeTopic, kRangeTopic, kOutPrioTopic, kChgPrioTopic,
    kTotBmsVTopic, kTotCellTopic, kCclTopic, kBatTempTopic,
    kPackTopics[0], kPackTopics[1], kPackTopics[2], kPackTopics[3],
    kPackTopics[4], kPackTopics[5], kPackTopics[6], kPackTopics[7],
};

size_t RelayController::inputTopicCount() { return sizeof(kInputTopics) / sizeof(kInputTopics[0]); }
const char* RelayController::inputTopic(size_t i) { return kInputTopics[i]; }

// Cherokee callbacks: one controller per board.
static Preferences*     s_prefs = nullptr;
static constexpr uint32_t kChCfgVersion = 1;
static void chLog(const char* line) { Log.println(line); }
static void chPersist(cherokee::Mode m, bool lock) {
    if (!s_prefs) return;
    s_prefs->putUChar("cmode", (uint8_t)m);
    s_prefs->putBool("clock", lock);
}

bool RelayController::handleMqttMessage(const String& topic, const String& payload) {
    return handleMqttMessage(topic.c_str(), payload.c_str());
}

void RelayController::begin(const Settings& s) {
    // Set the output latch to "off" before enabling the driver, so the relay
    // doesn't click on for a moment at boot.
    for (uint8_t pin : {kHeavyLoadPin, kGeneratorPin, kAux1Pin}) {
        digitalWrite(pin, kRelayOffLevel);
#if defined(ESP8266)
        pinMode(pin, OUTPUT);
#else
        pinMode(pin, kActiveHigh ? OUTPUT : OUTPUT_OPEN_DRAIN);
#endif
    }

    pinMode(kGenFeedbackPin, INPUT_PULLUP);

    _cfg = s;
    _prefs.begin("relays", false);
    _genFbEnabled = _prefs.getBool("gen_fb", false);
    // After a reboot the relays are decided from scratch: both start off and
    // the first fresh SOC decides (inside a hysteresis band = off). Only a
    // manual "forbid" survives — a reboot during maintenance must not switch
    // the heavy load or the generator back on. "Allow" / "run to goal" fall
    // back to auto.
    _heavyLatched = false;
    _genLatched   = false;
    uint8_t m     = _prefs.getUChar("gmode", (uint8_t)GenMode::Auto);
    _genMode      = m == (uint8_t)GenMode::Off ? GenMode::Off : GenMode::Auto;
    uint8_t hm    = _prefs.getUChar("hmode", (uint8_t)HeavyMode::Auto);
    _heavyMode    = hm == (uint8_t)HeavyMode::Off ? HeavyMode::Off : HeavyMode::Auto;
    if (_genMode != (GenMode)m || _heavyMode != (HeavyMode)hm) persist();
    if (_heavyMode == HeavyMode::Off) Log.println("[Relays] heavy load stays forbidden (manual, kept across reboot)");
    if (_genMode == GenMode::Off)     Log.println("[Relays] generator stays forbidden (manual, kept across reboot)");
    // A frozen BMS link usually survives our reboot; remember which SOC was
    // stuck so we don't need hours to detect it again.
    _frozenSoc    = _prefs.getInt("frozen_soc", -1);
    Log.printf("[Relays] start from scratch: both off until fresh SOC (genMode=%s heavyMode=%s frozenSoc=%d)\n",
               genModeName(_genMode), heavyModeName(_heavyMode), _frozenSoc);

    s_prefs = &_prefs;
    cherokee::Config cc = cherokee::defaults();
    cherokee::Config stored;
    if (_prefs.getUInt("chver", 0) == kChCfgVersion && _prefs.getBytesLength("chcfg") == sizeof(stored) &&
        _prefs.getBytes("chcfg", &stored, sizeof(stored)) == sizeof(stored) && !cherokee::validate(stored))
        cc = stored;
    uint8_t cm = _prefs.getUChar("cmode", (uint8_t)cherokee::Mode::Auto);
    if (cm > (uint8_t)cherokee::Mode::Off) cm = 0;
    _ch.begin(cc, (cherokee::Mode)cm, _prefs.getBool("clock", false), millis(), chLog, chPersist);

    applyOutputs();
}

bool RelayController::socFresh() const {
    return _haveSoc && millis() - _lastSocMs < kSocStaleMs;
}

float RelayController::reserveKwh() const {
    if (!_haveSoc) return -1;
    return _soc * _cfg.capacityKwh / 100.0f;
}

Season RelayController::activeSeason() const {
    switch (_cfg.seasonMode) {
        case SeasonMode::Summer:   return Season::Summer;
        case SeasonMode::Shoulder: return Season::Shoulder;
        case SeasonMode::Winter:   return Season::Winter;
        case SeasonMode::Auto:     break;
    }
    time_t now = time(nullptr);
    if (now < 1700000000) return Season::Winter;   // no NTP yet: strictest
    struct tm t;
    localtime_r(&now, &t);
    return settings::seasonForDate(t.tm_mon + 1, t.tm_mday);
}

// Solar elevation (NOAA approximation, good to a fraction of a degree).
static float sunElevationDeg(time_t now, float latDeg, float lonDeg) {
    struct tm u;
    gmtime_r(&now, &u);
    float hr = u.tm_hour + u.tm_min / 60.0f;
    float g = 2 * M_PI / 365 * (u.tm_yday + (hr - 12) / 24);
    float decl = 0.006918f - 0.399912f * cosf(g) + 0.070257f * sinf(g) - 0.006758f * cosf(2 * g)
               + 0.000907f * sinf(2 * g) - 0.002697f * cosf(3 * g) + 0.00148f * sinf(3 * g);
    float eqt = 229.18f * (0.000075f + 0.001868f * cosf(g) - 0.032077f * sinf(g)
               - 0.014615f * cosf(2 * g) - 0.040849f * sinf(2 * g));
    float ha = ((hr * 60 + eqt + 4 * lonDeg) / 4 - 180) * M_PI / 180;
    float lat = latDeg * M_PI / 180;
    return asinf(sinf(lat) * sinf(decl) + cosf(lat) * cosf(decl) * cosf(ha)) * 180 / M_PI;
}

bool RelayController::isNightNow() {
    time_t now = time(nullptr);
    if (now < 1700000000) return false;   // unknown time: don't claim night
    return sunElevationDeg(now, kSiteLat, kSiteLon) < -6.0f;
}

bool RelayController::frozenRulesPaused() const {
    // The generator charger and the Cherokee chargers feed the battery
    // behind the inverter's back.
    return generatorRunning() || (_genOffMs && millis() - _genOffMs < kGenSettleMs) || chargersRecent();
}

bool RelayController::chargersRecent() const {
    return _chEffective || (_chEffOffMs && millis() - _chEffOffMs < kGenSettleMs);
}

void RelayController::updateChargerDetect() {
    uint32_t now = millis();
    bool bmsFresh = _haveBatteryW && now - _lastBatteryWMs < kAuxStaleMs;
    bool invFresh = _lastInvBatIMs && now - _lastInvBatIMs < kAuxStaleMs && !_frozen;
    // Night only: by day the MakeSkyBlue MPPTs also charge the battery behind
    // the inverter's back and would look exactly like the generator charger.
    // Cherokee on the grid leave exactly the same signature: while they run
    // (and 30 min after) only our relay or the feedback contact count.
    bool seen = bmsFresh && invFresh && isNightNow() && !chargersRecent() &&
                (_batteryW - _invBatW) > kChargerDetectW;
    if (seen) {
        _chargerGoneMs = 0;
        if (!_chargerSinceMs) _chargerSinceMs = now;
        if (!_chargerDetected && now - _chargerSinceMs >= kChargerDetectMs) {
            _chargerDetected = true;
            Log.printf("[Relays] charger running (BMS %+.0f W vs inverter %+.0f W) -> heavy load OFF\n",
                       _batteryW, _invBatW);
            _changes++;
            applyOutputs();
        }
    } else {
        _chargerSinceMs = 0;
        if (_chargerDetected) {
            if (!_chargerGoneMs) _chargerGoneMs = now;
            if (now - _chargerGoneMs >= kChargerDetectMs) {
                _chargerDetected = false;
                _genOffMs = now;   // settle time for the frozen-SOC rules
                Log.println("[Relays] charger stopped");
                _changes++;
                applyOutputs();
            }
        }
    }
}

void RelayController::checkFrozen() {
    if (_frozen || !_haveSoc) return;
    bool night = _nightLoadWh >= kFrozenNightLoadWh;
    if (!night && fabsf(_invBatWh) < kFrozenInvBatWh) return;
    _frozen = true;
    _frozenSoc = _soc;
    _prefs.putInt("frozen_soc", _soc);
    // Shown on the web page and in Telegram, so in Ukrainian.
    _frozenReason = night
        ? "вночі без мережі будинок спожив " + String(_nightLoadWh, 0) + " Вт·год, а SOC не змінився"
        : "за інвертором через батарею пройшло " + String(fabsf(_invBatWh), 0) + " Вт·год, а SOC не змінився";
    Log.printf("[Relays] SOC looks FROZEN (%s) -> economy mode\n", _frozenReason.c_str());
    _changes++;
    applyOutputs();
}

void RelayController::onInverterBatteryVoltage(float v) {
    uint32_t now = millis();
    _invBatV = v;
    _lastInvBatVMs = now;
    if (v < kVoltEmergencyV) {
        _highVSinceMs = 0;
        if (!_lowVSinceMs) _lowVSinceMs = now;
        if (!_voltEmergency && now - _lowVSinceMs >= kVoltEmergencyMs) {
            _voltEmergency = true;
            Log.printf("[Relays] VOLTAGE EMERGENCY: battery %.1f V < %.1f V for %lu min\n",
                       v, kVoltEmergencyV, (unsigned long)(kVoltEmergencyMs / 60000));
            // Act through the latches so the decision survives a reboot and
            // isn't undone by an untrusted (possibly frozen high) SOC.
            _heavyLatched = false;
            if (_genMode != GenMode::Off) _genLatched = true;
            if (_genMode == GenMode::Run) _genMode = GenMode::Auto;
            persist();
            _changes++;
            applyOutputs();
        }
    } else {
        _lowVSinceMs = 0;
        if (_voltEmergency && v >= kVoltRecoverV) {
            if (!_highVSinceMs) _highVSinceMs = now;
            if (now - _highVSinceMs >= kVoltEmergencyMs) {
                _voltEmergency = false;
                Log.printf("[Relays] voltage emergency cleared (%.1f V)\n", v);
                _changes++;
            }
        }
    }
}

bool RelayController::handleMqttMessage(const char* topic, const char* payload) {
    float v;
    if (strcmp(topic, kLoadTopic) == 0) {
        if (!parseNumber(payload, v)) return true;
        uint32_t now = millis();
        bool gridFresh = _gridV >= 0 && now - _lastGridVMs < kAuxStaleMs;
        if (_lastLoadMs && gridFresh && _gridV < kGridPresentV && !frozenRulesPaused() && isNightNow()) {
            uint32_t dt = min<uint32_t>(now - _lastLoadMs, 60000);
            _nightLoadWh += max(_loadW, 0.0f) * dt / 3600000.0f;
            checkFrozen();
        }
        _loadW = v;
        _lastLoadMs = now;
        return true;
    }
    if (strcmp(topic, kGridVTopic) == 0) {
        if (!parseNumber(payload, v)) return true;
        _gridV = v;
        _lastGridVMs = millis();
        return true;
    }
    if (strcmp(topic, kInvBatVTopic) == 0) {
        if (parseNumber(payload, v)) onInverterBatteryVoltage(v);
        return true;
    }
    if (strcmp(topic, kInvBatITopic) == 0) {
        if (!parseNumber(payload, v)) return true;
        uint32_t now = millis();
        bool vFresh = _invBatV > 0 && now - _lastInvBatVMs < kAuxStaleMs;
        if (_lastInvBatIMs && vFresh && _haveSoc && _soc < 98 && !frozenRulesPaused()) {
            uint32_t dt = min<uint32_t>(now - _lastInvBatIMs, 60000);
            _invBatWh += _invBatV * v * dt / 3600000.0f;
            checkFrozen();
        }
        _lastInvBatIMs = now;
        _invBatI = v;
        if (vFresh) _invBatW = _invBatV * v;
        return true;
    }
    if (strcmp(topic, kBatteryPowerTopic) == 0) {
        if (!parseNumber(payload, v)) return true;
        uint32_t now = millis();
        // Integrate what actually reaches the battery while the generator
        // runs. Cap dt so a gap in data doesn't turn into a fake jump.
        if (_genOut && _haveBatteryW) {
            uint32_t dt = min<uint32_t>(now - _lastBatteryWMs, 60000);
            if (_batteryW > 0) _sessionKwh += _batteryW * dt / 3600000000.0f;
        }
        _batteryW = v;
        _lastBatteryWMs = now;
        _haveBatteryW = true;
        updateChargerDetect();
        return true;
    }
    if (handleCherokeeTopic(topic, payload)) return true;
    if (strcmp(topic, kSocTopic) != 0) return false;

    if (!parseNumber(payload, v) || v < 0 || v > 100) {
        Log.printf("[Relays] ignoring invalid SOC '%s'\n", payload);
        return true;
    }
    int newSoc = (int)v;
    if (!_haveSoc) {
        // Rebooted while SA was stuck: the first SOC we see is the stuck value.
        if (_frozenSoc >= 0 && newSoc == _frozenSoc) {
            _frozen = true;
            _frozenReason = "досі стоїть на тому ж значенні, що й до перезавантаження плати";
            Log.printf("[Relays] SOC %s -> economy mode\n", _frozenReason.c_str());
        } else if (_frozenSoc >= 0) {
            _frozenSoc = -1;
            _prefs.putInt("frozen_soc", -1);
        }
    } else if (newSoc != _soc) {
        _nightLoadWh = 0;
        _invBatWh = 0;
        if (_frozen) {
            Log.printf("[Relays] SOC moving again (%d -> %d), frozen state cleared\n", _soc, newSoc);
            _frozen = false;
            _frozenReason = "";
            _frozenSoc = -1;
            _prefs.putInt("frozen_soc", -1);
        }
    }
    if (newSoc != _soc) _changes++;
    _soc = newSoc;
    _lastSocMs = millis();
    _haveSoc = true;

    evaluate();
    return true;
}

void RelayController::evaluate() {
    if (!socTrusted()) { applyOutputs(); return; }

    const SeasonProfile& p = profile();
    float r = reserveKwh();
    bool heavyBefore = _heavyLatched, genBefore = _genLatched;
    GenMode modeBefore = _genMode;

    if (!_heavyLatched && r > p.heavyOnKwh)       _heavyLatched = true;
    else if (_heavyLatched && r < p.heavyOffKwh)  _heavyLatched = false;

    if (!p.genAuto)                               _genLatched = false;
    else if (!_genLatched && r < p.genStartKwh)   _genLatched = true;
    else if (_genLatched && r > p.genStopKwh)     _genLatched = false;

    if (_genMode == GenMode::Run && r >= p.genStopKwh) {
        Log.printf("[Relays] manual generator run reached goal %.1f kWh (reserve %.1f)\n",
                   p.genStopKwh, r);
        _genMode = GenMode::Auto;
    }

    if (_heavyLatched != heavyBefore || _genLatched != genBefore || _genMode != modeBefore) {
        persist();
        _changes++;
    }
    applyOutputs();
}

void RelayController::tick() {
    updateGenFeedback();
    tickCherokee();
    applyOutputs();
}

static cherokee::Tri triWord(const char* p, const char* yesPrefix) {
    if (!*p || strncmp(p, "unavailable", 11) == 0) return cherokee::Tri::Unknown;
    return strncmp(p, yesPrefix, strlen(yesPrefix)) == 0 ? cherokee::Tri::Yes : cherokee::Tri::No;
}

static cherokee::AcRange rangeWord(const char* p) {
    if (strncmp(p, "APL", 3) == 0) return cherokee::AcRange::Apl;
    if (strncmp(p, "UPS", 3) == 0) return cherokee::AcRange::Ups;
    return cherokee::AcRange::Unknown;
}

bool RelayController::handleCherokeeTopic(const char* topic, const char* payload) {
    float v;
    uint32_t now = millis();
    for (int i = 0; i < 8; i++) {
        if (strcmp(topic, kPackTopics[i]) != 0) continue;
        if (parseNumber(payload, v) && v > 0) {
            if (i % 2 == 0) { _packCellV[i / 2] = v; _packCellMs[i / 2] = now; }
            else            { _packV[i / 2] = v;     _packVMs[i / 2] = now; }
        }
        return true;
    }
    if (strcmp(topic, kPvTopic) == 0) {
        if (parseNumber(payload, v)) { _pvW = v; _pvMs = now; }
        return true;
    }
    if (strcmp(topic, kTotBmsVTopic) == 0) {
        if (parseNumber(payload, v) && v > 0) { _totBmsV = v; _totBmsVMs = now; }
        return true;
    }
    if (strcmp(topic, kTotCellTopic) == 0) {
        if (parseNumber(payload, v) && v > 0) { _totCellV = v; _totCellMs = now; }
        return true;
    }
    if (strcmp(topic, kCclTopic) == 0) {
        if (parseNumber(payload, v) && v >= 0) { _ccl = v; _cclMs = now; }
        return true;
    }
    if (strcmp(topic, kBatTempTopic) == 0) {
        if (parseNumber(payload, v) && v > -40 && v < 100) { _batTemp = v; _batTempMs = now; }
        return true;
    }
    if (strcmp(topic, kDevModeTopic) == 0) {
        _devGrid = triWord(payload, "Grid");
        _devModeMs = now;
        return true;
    }
    if (strcmp(topic, kChgPrioTopic) == 0) {
        // "Utility first", "Solar and utility simultaneously" allow the
        // grid; "Solar first" / "Only solar charging permitted" don't.
        _chgUtility = !*payload || strncmp(payload, "unavailable", 11) == 0 ? cherokee::Tri::Unknown
                    : (strstr(payload, "tility") ? cherokee::Tri::Yes : cherokee::Tri::No);
        _chgPrioMs = now;
        return true;
    }
    if (strcmp(topic, kOutPrioTopic) == 0) {
        _outPrio = strncmp(payload, "Utility first", 13) == 0 ? cherokee::OutPrio::Utility
                 : strncmp(payload, "Solar/Battery/Utility", 21) == 0 || strncmp(payload, "SBU", 3) == 0 ? cherokee::OutPrio::Sbu
                 : strncmp(payload, "Solar first", 11) == 0 ? cherokee::OutPrio::Solar
                 : cherokee::OutPrio::Unknown;
        _outPrioMs = now;
        return true;
    }
    if (strcmp(topic, kRangeTopic) == 0) {
        _range = rangeWord(payload);
        _rangeMs = now;
        return true;
    }
    return false;
}

void RelayController::onHint(const char* json, unsigned len) {
    StaticJsonDocument<384> d;
    if (deserializeJson(d, json, len)) {
        Log.println("[Cherokee] bad HA hint (not JSON), ignored");
        return;
    }
    uint32_t now = millis();
    uint32_t ageMs = 0;
    long ts = d["ts"] | 0L;
    time_t t = time(nullptr);
    if (ts > 1700000000 && t > 1700000000) {
        long age = (long)t - ts;
        if (age < 0) age = 0;
        if ((uint32_t)age * 1000UL >= kHintTtlMs) {
            Log.printf("[Cherokee] HA hint is %ld min old, ignored\n", age / 60);
            return;
        }
        ageMs = (uint32_t)age * 1000UL;
    }
    const char* adv = d["chargers_advice"] | "";
    _hintAdvice = !strcmp(adv, "on") ? cherokee::Tri::Yes : !strcmp(adv, "off") ? cherokee::Tri::No : cherokee::Tri::Unknown;
    _hintSurplus = d["surplus"] | false;
    _hintOutageMin = d["outage_soon_min"] | -1;
    _hintRange = rangeWord(d["ac_range"] | "");
    _hintMs = now - ageMs;
    _haveHint = true;
    Log.printf("[Cherokee] HA hint: advice=%s surplus=%d outage_in=%d min range=%s\n", adv, (int)_hintSurplus,
               _hintOutageMin, d["ac_range"] | "?");
    _changes++;
}

static bool freshMs(uint32_t ms, uint32_t ttl) { return ms && millis() - ms < ttl; }

void RelayController::tickCherokee() {
    uint32_t now = millis();
    if (_lastChEvalMs && now - _lastChEvalMs < 1000) return;
    _lastChEvalMs = now;

    // Sun position, once a minute: day = above 5 deg; minutes until it sets.
    time_t tnow = time(nullptr);
    bool timeKnown = tnow > 1700000000;
    if (!_sunCalcDone || now - _sunCalcMs >= 60000) {
        _sunCalcDone = true;
        _sunCalcMs = now;
        _day = false;
        _toSunsetMin = -1;
        if (timeKnown) {
            float el = sunElevationDeg(tnow, kSiteLat, kSiteLon);
            _day = el > 5.0f;
            if (el > 0) {
                for (int m = 5; m <= 16 * 60; m += 5) {
                    if (sunElevationDeg(tnow + m * 60, kSiteLat, kSiteLon) <= 0) { _toSunsetMin = m; break; }
                }
            }
        }
    }

    cherokee::Inputs in;
    in.now = now;
    in.season = (uint8_t)activeSeason();
    in.seasonCertain = timeKnown || _cfg.seasonMode != SeasonMode::Auto;
    in.socFresh = socFresh();
    in.socFrozen = _frozen;
    in.soc = _soc;
    in.voltEmergency = _voltEmergency;

    // Highest cell / BMS voltage: the total topic if SA ever publishes it,
    // otherwise the maximum over the packs heard recently.
    float cell = 0, bmsV = 0;
    if (freshMs(_totCellMs, kAuxStaleMs)) cell = _totCellV;
    if (freshMs(_totBmsVMs, kAuxStaleMs)) bmsV = _totBmsV;
    for (int i = 0; i < 4; i++) {
        if (freshMs(_packCellMs[i], kAuxStaleMs) && _packCellV[i] > cell) cell = _packCellV[i];
        if (freshMs(_packVMs[i], kAuxStaleMs) && _packV[i] > bmsV) bmsV = _packV[i];
    }
    in.cellFresh = cell > 0;   in.cellMaxV = cell;
    in.bmsVFresh = bmsV > 0;   in.bmsV = bmsV;
    in.cclFresh = freshMs(_cclMs, kAuxStaleMs);         in.ccl = _ccl;
    in.tempFresh = freshMs(_batTempMs, kAuxStaleMs);    in.tempC = _batTemp;
    in.invIFresh = freshMs(_lastInvBatIMs, kAuxStaleMs); in.invBatI = _invBatI;
    in.gridFresh = _gridV >= 0 && freshMs(_lastGridVMs, kAuxStaleMs); in.gridV = _gridV;
    in.pvFresh = freshMs(_pvMs, kAuxStaleMs);           in.pvW = _pvW;
    in.loadFresh = freshMs(_lastLoadMs, kAuxStaleMs);   in.loadW = _loadW;
    in.extFresh = _haveBatteryW && freshMs(_lastBatteryWMs, kAuxStaleMs) && freshMs(_lastInvBatIMs, kAuxStaleMs);
    in.extDcW = _batteryW - _invBatW;
    in.batV = in.bmsVFresh ? bmsV : (_invBatV > 0 ? _invBatV : 0);
    in.gridMode = freshMs(_devModeMs, kSettingStaleMs) ? _devGrid : cherokee::Tri::Unknown;
    in.chargerUtility = freshMs(_chgPrioMs, kSettingStaleMs) ? _chgUtility : cherokee::Tri::Unknown;
    in.outPrio = freshMs(_outPrioMs, kSettingStaleMs) ? _outPrio : cherokee::OutPrio::Unknown;
    bool hintFresh = _haveHint && now - _hintMs < kHintTtlMs;
    in.range = freshMs(_rangeMs, kSettingStaleMs) && _range != cherokee::AcRange::Unknown ? _range
             : hintFresh ? _hintRange : cherokee::AcRange::Unknown;
    in.timeKnown = timeKnown;
    in.day = _day;
    in.toSunsetMin = _toSunsetMin;
    if (hintFresh) {
        in.adviceOn = _hintAdvice;
        in.surplus = _hintSurplus;
        if (_hintOutageMin >= 0) {
            int left = _hintOutageMin - (int)((now - _hintMs) / 60000);
            in.outageSoonMin = left >= 0 ? left : -1;
        }
    }
    _lastIn = in;

    bool wasOn = _ch.chargersOn();
    const char* wasReason = _ch.reason();
    _ch.update(in);
    if (_ch.chargersOn() != wasOn || _ch.reason() != wasReason) _changes++;

    // "Effectively charging": commanded on and the grid there to feed the
    // coil. A stale grid reading counts as present (the cautious side).
    bool eff = _ch.chargersOn() && (!in.gridFresh || in.gridV >= kGridPresentV);
    if (_chEffective && !eff) _chEffOffMs = now;
    _chEffective = eff;
}

void RelayController::setGenFeedbackEnabled(bool on) {
    if (on == _genFbEnabled) return;
    _genFbEnabled = on;
    _prefs.putBool("gen_fb", on);
    _genNoStart = false;
    _genNotRunningSinceMs = 0;
    Log.printf("[Relays] generator feedback contact %s\n", on ? "enabled" : "disabled");
    _changes++;
    applyOutputs();
}

void RelayController::updateGenFeedback() {
    uint32_t now = millis();
    bool raw = digitalRead(kGenFeedbackPin) == LOW;   // contact closed = running
    if (raw != _genFbRaw) { _genFbRaw = raw; _genFbRawSinceMs = now; }
    if (_genFbEnabled && raw != _genFbRunning && now - _genFbRawSinceMs >= kGenFeedbackDebounceMs) {
        _genFbRunning = raw;
        Log.printf("[Relays] generator feedback: %s%s\n", raw ? "RUNNING" : "stopped",
                   raw && !_genOut ? " (not started by the board)" : "");
        if (!raw) _genOffMs = now;   // settle time for the frozen-SOC rules
        _changes++;
    }
    if (!_genFbEnabled) { _genFbRunning = false; _genNoStart = false; return; }

    // Relay closed but the generator isn't (or is no longer) running.
    if (_genOut && !_genFbRunning) {
        if (!_genNotRunningSinceMs) _genNotRunningSinceMs = now;
        if (!_genNoStart && now - _genNotRunningSinceMs >= kGenNoStartMs) {
            _genNoStart = true;
            Log.println("[Relays] generator commanded but NOT running (no feedback for 3 min)");
            _changes++;
        }
    } else {
        _genNotRunningSinceMs = 0;
        if (_genNoStart) { _genNoStart = false; _changes++; }
    }
}

void RelayController::applyOutputs() {
    bool fresh = socFresh();
    if (_wasFresh != fresh) {
        if (!fresh) Log.printf("[Relays] no SOC for %lu s -> economy mode\n",
                               (unsigned long)(kSocStaleMs / 1000));
        else        Log.printf("[Relays] SOC data fresh (soc=%d)\n", _soc);
        _wasFresh = fresh;
        _changes++;
    }

    // No heavy load while the generator runs (its ~1 kW charger can't feed
    // it) or in a voltage emergency — in every mode, manual ones included.
    bool blocked = _voltEmergency || generatorRunning();
    bool heavy;
    switch (_heavyMode) {
        case HeavyMode::On:  heavy = !blocked; break;
        case HeavyMode::Off: heavy = false; break;
        default:             heavy = _heavyLatched && fresh && !_frozen && !blocked; break;
    }
    bool gen   = _genMode == GenMode::Run || (_genMode == GenMode::Auto && _genLatched);

    if (heavy != _heavyOut) {
        Log.printf("[Relays] heavy load -> %s (soc=%d, reserve=%.1f kWh, mode=%s, %s)\n",
                   heavy ? "ON" : "OFF", _soc, reserveKwh(), heavyModeName(_heavyMode),
                   settings::seasonName(activeSeason()));
        _heavyOut = heavy;
        _changes++;
    }
    if (gen != _genOut) {
        Log.printf("[Relays] generator -> %s (soc=%d, reserve=%.1f kWh, mode=%s, %s)\n",
                   gen ? "ON" : "OFF", _soc, reserveKwh(), genModeName(_genMode),
                   settings::seasonName(activeSeason()));
        if (gen) {
            _genOnSinceMs = millis();
            _sessionKwh = 0;
            _nightLoadWh = 0;
            _invBatWh = 0;
        } else {
            Log.printf("[Relays] generator session: %.2f kWh into the battery in %lu min\n",
                       _sessionKwh, (unsigned long)((millis() - _genOnSinceMs) / 60000));
            _genOffMs = millis();
        }
        _genOut = gen;
        _changes++;
    }
    writeRelay(kHeavyLoadPin, _heavyOut);
    writeRelay(kGeneratorPin, _genOut);
    // NC wiring: energise the relay to switch the chargers OFF.
    writeRelay(kAux1Pin, !_ch.chargersOn());
}

void RelayController::setCherokeeMode(cherokee::Mode m, const char* source) {
    _ch.setMode(m, source);
    _lastChEvalMs = 0;   // decide right away, not within a second
    tickCherokee();
    _changes++;
    applyOutputs();
}

bool RelayController::cherokeeAck(const char* source) {
    bool r = _ch.ack(source);
    if (r) {
        _lastChEvalMs = 0;
        tickCherokee();
        _changes++;
        applyOutputs();
    }
    return r;
}

void RelayController::setCherokeeConfig(const cherokee::Config& c) {
    _prefs.putBytes("chcfg", &c, sizeof(c));
    _prefs.putUInt("chver", kChCfgVersion);
    _ch.setConfig(c);
    Log.println("[Cherokee] settings saved");
    _changes++;
}

void RelayController::persist() {
    _prefs.putUChar("gmode", (uint8_t)_genMode);
    _prefs.putUChar("hmode", (uint8_t)_heavyMode);
}

void RelayController::setHeavyMode(HeavyMode m, const char* source) {
    if (m == _heavyMode) return;
    Log.printf("[Relays] heavy load mode %s -> %s (by %s)\n",
               heavyModeName(_heavyMode), heavyModeName(m), source);
    _heavyMode = m;
    persist();
    _changes++;
    applyOutputs();
}

const char* RelayController::heavyModeName(HeavyMode m) {
    switch (m) {
        case HeavyMode::Auto: return "auto";
        case HeavyMode::On:   return "on";
        case HeavyMode::Off:  return "off";
    }
    return "?";
}

bool RelayController::parseHeavyMode(const String& s, HeavyMode& out) {
    if (s == "auto") { out = HeavyMode::Auto; return true; }
    if (s == "on")   { out = HeavyMode::On;   return true; }
    if (s == "off")  { out = HeavyMode::Off;  return true; }
    return false;
}

void RelayController::applySettings(const Settings& s) {
    _cfg = s;
    _changes++;
    evaluate();
}

void RelayController::setGenMode(GenMode m, const char* source) {
    if (m == _genMode) return;
    Log.printf("[Relays] generator mode %s -> %s (by %s)\n",
               genModeName(_genMode), genModeName(m), source);
    _genMode = m;
    // Switching to Auto drops any stale latch; evaluate() re-derives it.
    if (m == GenMode::Auto) _genLatched = false;
    persist();
    _changes++;
    evaluate();
}

void RelayController::resetState() {
    Log.println("[Relays] state reset: relays off, generator mode auto, Cherokee auto");
    _ch.setMode(cherokee::Mode::Auto, "reset");
    _heavyLatched = false;
    _genLatched = false;
    _genMode = GenMode::Auto;
    persist();
    _changes++;
    evaluate();
}

float RelayController::genEtaHours() const {
    if (!_genOut || !socTrusted() || !_haveBatteryW) return -1;
    if (millis() - _lastBatteryWMs > kAuxStaleMs || _batteryW < 50) return -1;
    float left = profile().genStopKwh - reserveKwh();
    if (left <= 0) return 0;
    return left / (_batteryW / 1000.0f);
}

// Floats go out as doubles (9.6f -> 9.600000381); round for display.
static double r2(float v) { return round(v * 100.0) / 100.0; }

void RelayController::fillStatus(JsonObject o, bool full) const {
    o["soc"] = _soc;
    o["soc_fresh"] = socFresh();
    o["soc_age_s"] = _haveSoc ? (long)((millis() - _lastSocMs) / 1000) : -1L;
    o["soc_frozen"] = _frozen;
    o["frozen_reason"] = _frozenReason;
    o["frozen_night_wh"] = (int)_nightLoadWh;
    o["frozen_inv_wh"] = (int)fabsf(_invBatWh);
    o["frozen_paused"] = frozenRulesPaused();
    o["night"] = isNightNow();
    o["inv_battery_v"] = r2(_invBatV);
    o["volt_emergency"] = _voltEmergency;
    o["relay_active_high"] = kActiveHigh;
#if defined(ESP8266)
    o["pin_heavy"] = "D1 (GPIO5)";
    o["pin_gen"] = "D2 (GPIO4)";
    o["pin_aux1"] = "D6 (GPIO12)";
#else
    o["pin_heavy"] = "GPIO25";
    o["pin_gen"] = "GPIO26";
    o["pin_aux1"] = "GPIO33";
#endif
    o["charger_detected"] = _chargerDetected;
    o["gen_fb_enabled"] = _genFbEnabled;
    o["gen_fb_running"] = _genFbRunning;
    o["gen_no_start"] = _genNoStart;
    o["charger_w"] = (int)(_batteryW - _invBatW);
    o["grid_v"] = r2(_gridV);
    o["load_w"] = (int)_loadW;
    o["reserve_kwh"] = r2(reserveKwh());
    o["capacity_kwh"] = r2(_cfg.capacityKwh);
    o["season"] = settings::seasonName(activeSeason());
    o["season_mode"] = settings::seasonModeName(_cfg.seasonMode);
    o["heavy_load"] = _heavyOut;
    o["generator"] = _genOut;
    fillCherokee(o, full);
    o["gen_mode"] = genModeName(_genMode);
    o["heavy_mode"] = heavyModeName(_heavyMode);
    o["battery_w"] = _haveBatteryW ? (int)_batteryW : 0;
    o["gen_on_min"] = _genOut ? (millis() - _genOnSinceMs) / 60000 : 0;
    o["gen_session_kwh"] = r2(_sessionKwh);
    o["gen_eta_h"] = round(genEtaHours() * 10.0) / 10.0;
    const SeasonProfile& p = profile();
    JsonObject th = o.createNestedObject("thresholds");
    th["heavy_on_kwh"] = r2(p.heavyOnKwh);
    th["heavy_off_kwh"] = r2(p.heavyOffKwh);
    th["gen_auto"] = p.genAuto;
    th["gen_start_kwh"] = r2(p.genStartKwh);
    th["gen_stop_kwh"] = r2(p.genStopKwh);
}

static const char* triName(cherokee::Tri t) {
    return t == cherokee::Tri::Yes ? "yes" : t == cherokee::Tri::No ? "no" : "?";
}

void RelayController::fillCherokee(JsonObject o, bool full) const {
    const cherokee::Inputs& in = _lastIn;
    uint32_t now = millis();
    o["cherokee"] = _ch.chargersOn() ? "on" : "off";   // strings: the bridge compares them with command args
    o["cherokee_mode"] = cherokee::Controller::modeName(_ch.mode());
    o["cherokee_reason"] = _ch.reason();
    o["cherokee_hold_s"] = _ch.holding() ? _ch.holdLeftS() : 0;
    o["cherokee_switches_1h"] = _ch.switchesLastHour(now);
    o["cherokee_emerg"] = _ch.emergencyOff() ? cherokee::Controller::causeName(_ch.emergencyCause()) : "";
    o["cherokee_lock"] = _ch.lockout();
    o["cherokee_flap"] = _ch.flapLocked();
    o["inv_can_charge"] = _ch.invCanCharge();
    o["ext_dc_w"] = in.extFresh ? (int)in.extDcW : 0;
    o["sun_w"] = (int)_ch.sunNowW();
    o["pv_w"] = in.pvFresh ? (int)in.pvW : -1;
    o["cell_max_v"] = in.cellFresh ? round(in.cellMaxV * 1000.0) / 1000.0 : -1;
    o["bms_v"] = in.bmsVFresh ? r2(in.bmsV) : -1;
    o["ac_range"] = in.range == cherokee::AcRange::Apl ? "APL" : in.range == cherokee::AcRange::Ups ? "UPS" : "?";
    if (!full) return;   // the rest is for the web page only (HA state stays small)
    o["cherokee_min"] = _ch.sinceChangeS(now) / 60;
    o["cherokee_trips_24h"] = _ch.emergTrips24h(now);
    o["cherokee_full"] = _ch.full();
    o["cherokee_effective"] = _chEffective;
    o["inv_check"] = _ch.verifying();
    o["apl_hold"] = _ch.aplHold();
    o["sun_enough"] = _ch.sunEnough();
    o["ccl_a"] = in.cclFresh ? r2(in.ccl) : -1;
    o["bat_temp"] = in.tempFresh ? r2(in.tempC) : -99;
    o["ac_range_src"] = freshMs(_rangeMs, kSettingStaleMs) && _range != cherokee::AcRange::Unknown ? "sa"
                      : in.range != cherokee::AcRange::Unknown ? "ha" : "";
    o["out_prio"] = in.outPrio == cherokee::OutPrio::Utility ? "utility" : in.outPrio == cherokee::OutPrio::Sbu ? "sbu"
                  : in.outPrio == cherokee::OutPrio::Solar ? "solar" : "?";
    o["chg_utility"] = triName(in.chargerUtility);
    o["dev_grid"] = triName(in.gridMode);
    o["day"] = in.day;
    o["to_sunset_min"] = in.toSunsetMin;
    o["hint_age_min"] = _haveHint ? (long)((now - _hintMs) / 60000) : -1L;
    o["hint_advice"] = triName(in.adviceOn);
    o["hint_surplus"] = in.surplus;
    o["hint_outage_min"] = in.outageSoonMin;
    const cherokee::Profile& p = _ch.config().p[in.season < 3 ? in.season : 2];
    JsonObject t = o.createNestedObject("ch_th");
    t["on"] = p.onSoc;
    t["off"] = p.offSoc;
    t["floor"] = p.floorSoc;
    t["sunset"] = p.sunsetSoc;
    t["advice_on"] = _ch.config().adviceOnSoc;
    t["cell_cap"] = r2(_ch.config().cellCapV);
}

String RelayController::legacyStatusJson() const {
    String j = "{";
    j += "\"time\":\"" + String(millis() / 1000) + "s\",";
    j += "\"soc\":" + String(_soc) + ",";
    j += "\"relay1\":\"" + String(_heavyOut ? "ON" : "OFF") + "\",";
    j += "\"relay2\":\"" + String(_genOut ? "ON" : "OFF") + "\",";
    j += "\"comment\":\"When Relay1 is ON, 24V power supply should be ON even if 220 main contactor is OFF\"";
    j += "}";
    return j;
}

const char* RelayController::genModeName(GenMode m) {
    switch (m) {
        case GenMode::Auto: return "auto";
        case GenMode::Run:  return "run";
        case GenMode::Off:  return "off";
    }
    return "?";
}

bool RelayController::parseGenMode(const String& s, GenMode& out) {
    if (s == "auto") { out = GenMode::Auto; return true; }
    if (s == "run")  { out = GenMode::Run;  return true; }
    if (s == "off")  { out = GenMode::Off;  return true; }
    return false;
}
