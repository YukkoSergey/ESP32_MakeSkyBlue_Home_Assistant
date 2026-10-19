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

// Same freshness rule for the auxiliary readings as for SOC.
static constexpr uint32_t kAuxStaleMs = RelayController::kSocStaleMs;

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
};

size_t RelayController::inputTopicCount() { return sizeof(kInputTopics) / sizeof(kInputTopics[0]); }
const char* RelayController::inputTopic(size_t i) { return kInputTopics[i]; }

bool RelayController::handleMqttMessage(const String& topic, const String& payload) {
    return handleMqttMessage(topic.c_str(), payload.c_str());
}

void RelayController::begin(const Settings& s) {
    // Set the output latch to "off" before enabling the driver, so the relay
    // doesn't click on for a moment at boot.
    for (uint8_t pin : {kHeavyLoadPin, kGeneratorPin}) {
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
    // The generator charger feeds the battery behind the inverter's back.
    return generatorRunning() || (_genOffMs && millis() - _genOffMs < kGenSettleMs);
}

void RelayController::updateChargerDetect() {
    uint32_t now = millis();
    bool bmsFresh = _haveBatteryW && now - _lastBatteryWMs < kAuxStaleMs;
    bool invFresh = _lastInvBatIMs && now - _lastInvBatIMs < kAuxStaleMs && !_frozen;
    // Night only: by day the MakeSkyBlue MPPTs also charge the battery behind
    // the inverter's back and would look exactly like the generator charger.
    bool seen = bmsFresh && invFresh && isNightNow() && (_batteryW - _invBatW) > kChargerDetectW;
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
    applyOutputs();
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
    Log.println("[Relays] state reset: both relays off, generator mode auto");
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

void RelayController::fillStatus(JsonObject o) const {
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
#else
    o["pin_heavy"] = "GPIO25";
    o["pin_gen"] = "GPIO26";
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
