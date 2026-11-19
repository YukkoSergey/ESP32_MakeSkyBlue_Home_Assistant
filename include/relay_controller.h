#ifndef RELAY_CONTROLLER_H
#define RELAY_CONTROLLER_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include "compat.h"
#include "settings.h"
#include "cherokee.h"

// -----------------------------------------------------------------------------
// Two active-LOW relays driven by battery reserve from Solar Assistant
// (replaces the ESP8266 NodeMCU sketch in the shed):
//
//   * Heavy load — powers the 24 V PSU whose signal line enables the contactor
//                  in the house (boiler, floor heating, AC / heat pump may run
//                  from the batteries). Hysteresis on reserve kWh.
//   * Generator  — 2-wire autostart, contact held closed = generator runs.
//   * Relay 3    — Cherokee chargers, wired on the NC contact: relay released =
//                  chargers ON (see cherokee.h for the whole logic).
//
// Thresholds come from the active season profile (see settings.h).
//
// Generator modes:
//   * Auto — start/stop by the season profile (or never, if genAuto=false).
//   * Run  — manual start; stops by itself once reserve reaches the profile's
//            genStopKwh, then falls back to Auto.
//   * Off  — inhibited until set back to Auto.
//
// Fail-safe rules:
//   * No fresh SOC for kSocStaleMs -> heavy load OFF (economy mode). The
//     generator keeps its state: a running one charges until it runs out of
//     fuel, a stopped one isn't started blind.
//   * Payloads outside 0..100 are ignored and never drive the relays.
//   * Unknown date (no NTP yet) with season mode Auto -> winter profile.
//   * After a reboot both relays start off and are decided from scratch by
//     the first fresh SOC (inside a hysteresis band = off). Only a manual
//     "forbid" (heavy load or generator) is kept across reboots.
//
// Frozen SOC: SA can keep publishing a stuck SOC when its link to the
// Pylontech BMS breaks (seen 7 times in the 2024–2026 history, up to 58 days).
// The inverter measures the battery independently, so SOC is declared frozen
// when, since its last change:
//   * at night with no grid voltage the house used >= kFrozenNightLoadWh
//     (all of it must come from the battery — the MakeSkyBlue chargers are
//     dark and SA can't see them anyway), any SOC including 100 %; or
//   * below 98 % SOC the inverter-measured battery energy (V x I) reached
//     kFrozenInvBatWh (above 98 % the inverter shows a phantom ~100 W
//     discharge, so this rule is off there).
// Both rules pause while the generator runs and kGenSettleMs after: its
// charger adds energy the inverter doesn't see. Frozen = treated like stale
// data (heavy load OFF, generator unchanged) until SOC moves again.
//
// Generator running (our relay closed, or the charger seen in the data —
// it can also be started by hand or by its own controller) -> heavy load OFF
// at any SOC: the ~1 kW charger can't feed heavy loads, they would drain
// the battery it is trying to fill. The charger is seen as BMS battery power
// exceeding the inverter-measured battery power by kChargerDetectW for
// kChargerDetectMs (the inverter doesn't see the charger's current) — at night
// only, since by day the MakeSkyBlue MPPTs do the same.
//
// Generator feedback (optional, enabled in the web UI): a dry auxiliary
// contact of the generator's contactor between kGenFeedbackPin and GND,
// closed = generator really running. With it, "generator running" is the
// truth rather than a guess, and a relay closed for kGenNoStartMs without
// the contact closing raises "generator did not start / stopped by itself".
//
// Voltage emergency: inverter battery voltage below kVoltEmergencyV for
// kVoltEmergencyMs means the battery really is near empty whatever SOC says
// (history: only at SOC 4 % and 11 %). Heavy load OFF and generator START in
// any season unless it is inhibited by hand.
// -----------------------------------------------------------------------------
enum class GenMode : uint8_t { Auto = 0, Run = 1, Off = 2 };
// Heavy load: Auto = by the season profile; On = allowed regardless of the
// thresholds; Off = forbidden. Voltage emergency and a running generator cut
// it in every mode — those protect the battery, they aren't preferences.
enum class HeavyMode : uint8_t { Auto = 0, On = 1, Off = 2 };

class RelayController {
public:
#if defined(ESP8266)
    static constexpr uint8_t  kHeavyLoadPin = 5;    // NodeMCU D1
    static constexpr uint8_t  kGeneratorPin = 4;    // NodeMCU D2
    static constexpr uint8_t  kAux1Pin      = 12;   // NodeMCU D6, relay 3 (Cherokee, NC)
#else
    static constexpr uint8_t  kHeavyLoadPin = 25;   // was D1 (GPIO5) on the NodeMCU
    static constexpr uint8_t  kGeneratorPin = 26;   // was D2 (GPIO4) on the NodeMCU
    static constexpr uint8_t  kAux1Pin      = 33;
#endif

#ifdef RELAY_SOC_STALE_MS
    static constexpr uint32_t kSocStaleMs  = RELAY_SOC_STALE_MS;   // bench testing
#else
    static constexpr uint32_t kSocStaleMs  = 3UL * 60UL * 1000UL;
#endif
    static constexpr float    kFrozenNightLoadWh = 1000;
    static constexpr float    kFrozenInvBatWh    = 1500;
    static constexpr uint32_t kGenSettleMs       = 30UL * 60UL * 1000UL;
    static constexpr float    kVoltEmergencyV    = 47.0f;
    static constexpr float    kVoltRecoverV      = 49.0f;
#ifdef RELAY_VOLT_EMERGENCY_MS
    static constexpr uint32_t kVoltEmergencyMs   = RELAY_VOLT_EMERGENCY_MS;   // bench testing
#else
    static constexpr uint32_t kVoltEmergencyMs   = 5UL * 60UL * 1000UL;
#endif
    static constexpr float    kGridPresentV      = 100.0f;
    static constexpr float    kChargerDetectW    = 300.0f;
    static constexpr uint32_t kChargerDetectMs   = 2UL * 60UL * 1000UL;
    // Night = sun below -6 deg (civil dusk) at the site.
    static constexpr float    kSiteLat = 50.45f, kSiteLon = 30.52f;   // Kyiv region

#if defined(ESP8266)
    static constexpr uint8_t  kGenFeedbackPin = 14;   // NodeMCU D5
#else
    static constexpr uint8_t  kGenFeedbackPin = 27;
#endif
    static constexpr uint32_t kGenFeedbackDebounceMs = 2000;
    static constexpr uint32_t kGenNoStartMs = 3UL * 60UL * 1000UL;

    void begin(const Settings& s);
    // Returns true if the topic was consumed.
    bool handleMqttMessage(const char* topic, const char* payload);
    bool handleMqttMessage(const String& topic, const String& payload);
    // The exact SA topics this controller listens to (subscribe to these,
    // not to a wildcard).
    static size_t inputTopicCount();
    static const char* inputTopic(size_t i);
    void tick();

    void applySettings(const Settings& s);
    const Settings& currentSettings() const { return _cfg; }
    void setGenMode(GenMode m, const char* source);
    void setHeavyMode(HeavyMode m, const char* source);
    HeavyMode heavyMode() const { return _heavyMode; }
    static const char* heavyModeName(HeavyMode m);
    static bool parseHeavyMode(const String& s, HeavyMode& out);
    // Forget latched relay state and generator mode (back to Auto, both off).
    void resetState();
    // Cherokee chargers (relay 3, NC contact: relay energised = chargers OFF).
    void setCherokeeMode(cherokee::Mode m, const char* source);
    bool cherokeeAck(const char* source);
    const cherokee::Controller& cherokee() const { return _ch; }
    void setCherokeeConfig(const cherokee::Config& c);
    // Optional hints from Home Assistant (dacha_relays/hint, JSON, TTL 3 h).
    void onHint(const char* json, unsigned len);
    // Chargers commanded on with grid present, or less than 30 min ago:
    // they charge the battery behind the inverter's back.
    bool chargersRecent() const;

    GenMode genMode() const     { return _genMode; }
    bool heavyLoadOn() const    { return _heavyOut; }
    bool generatorOn() const    { return _genOut; }
    bool socFresh() const;
    bool socFrozen() const      { return _frozen; }
    // Fresh and not frozen: the only state in which SOC drives decisions.
    bool socTrusted() const     { return socFresh() && !_frozen; }
    bool voltEmergency() const  { return _voltEmergency; }
    // Charger (generator) seen in the data, whoever started it.
    bool chargerDetected() const { return _chargerDetected; }
    // With feedback: the contact (or our own start command, while it cranks).
    // Without: our relay, or the charger seen in the data at night.
    bool generatorRunning() const {
        return _genFbEnabled ? (_genFbRunning || _genOut) : (_genOut || _chargerDetected);
    }
    bool genFeedbackEnabled() const { return _genFbEnabled; }
    bool genFeedbackRunning() const { return _genFbRunning; }
    // Relay closed for kGenNoStartMs but the contact never closed (or opened).
    bool genNoStartAlarm() const { return _genNoStart; }
    void setGenFeedbackEnabled(bool on);
    const String& frozenReason() const { return _frozenReason; }
    static bool isNightNow();
    int  soc() const            { return _soc; }
    float reserveKwh() const;
    Season activeSeason() const;
    // Hours until a running generator reaches its goal at the current charge
    // rate; negative if unknown.
    float genEtaHours() const;

    // full = false: the compact set for the HA state topic.
    void fillStatus(JsonObject o, bool full = true) const;
    // Old ESP8266 endpoint format, kept for anything that still polls it.
    String legacyStatusJson() const;

    static const char* genModeName(GenMode m);
    static bool parseGenMode(const String& s, GenMode& out);

    // Bumped whenever something visible changes; lets the HA publisher know
    // when to push state early.
    uint32_t changeCounter() const { return _changes; }

private:
    void evaluate();
    void applyOutputs();
    void persist();
    bool frozenRulesPaused() const;
    void checkFrozen();
    void onInverterBatteryVoltage(float v);
    void updateChargerDetect();
    void updateGenFeedback();
    void tickCherokee();
    bool handleCherokeeTopic(const char* topic, const char* payload);
    void fillCherokee(JsonObject o, bool full) const;
    const SeasonProfile& profile() const { return _cfg.profiles[(int)activeSeason()]; }

    Settings    _cfg;
    Preferences _prefs;

    int      _soc       = -1;
    uint32_t _lastSocMs = 0;
    bool     _haveSoc   = false;
    bool     _wasFresh  = false;

    float    _batteryW        = 0;     // SA battery_power, + = charging
    uint32_t _lastBatteryWMs  = 0;
    bool     _haveBatteryW    = false;

    // Inverter-side readings (independent of the BMS link).
    float    _loadW = 0;        uint32_t _lastLoadMs = 0;
    float    _gridV = -1;       uint32_t _lastGridVMs = 0;
    float    _invBatV = -1;     uint32_t _lastInvBatVMs = 0;
    uint32_t _lastInvBatIMs = 0;
    float    _invBatW = 0;      // V x I from the inverter, + = charge

    // External charger detection.
    uint32_t _chargerSinceMs = 0;
    uint32_t _chargerGoneMs  = 0;
    bool     _chargerDetected = false;

    // Frozen-SOC detector: energy since the last SOC change.
    float    _nightLoadWh = 0;
    float    _invBatWh    = 0;   // signed, + = charge
    bool     _frozen      = false;
    int      _frozenSoc   = -1;
    String   _frozenReason;
    uint32_t _genOffMs    = 0;

    // Voltage emergency.
    uint32_t _lowVSinceMs  = 0;
    uint32_t _highVSinceMs = 0;
    bool     _voltEmergency = false;

    // Hysteresis state (what the profile says we want), persisted.
    bool     _heavyLatched = false;
    bool     _genLatched   = false;
    GenMode  _genMode      = GenMode::Auto;
    HeavyMode _heavyMode   = HeavyMode::Auto;

    // What's actually on the pins.
    bool     _heavyOut = false;
    bool     _genOut   = false;

    // Current / last generator session.
    uint32_t _genOnSinceMs  = 0;
    float    _sessionKwh    = 0;

    // Generator feedback contact.
    bool     _genFbEnabled = false;
    bool     _genFbRunning = false;
    bool     _genFbRaw = false;
    uint32_t _genFbRawSinceMs = 0;
    uint32_t _genNotRunningSinceMs = 0;
    bool     _genNoStart = false;

    // Cherokee inputs (LAN SA) and state.
    cherokee::Controller _ch;
    uint32_t _lastChEvalMs = 0;
    bool     _chEffective = true;    // relay released at boot: chargers on
    uint32_t _chEffOffMs = 0;
    bool     _chLastOn = true;
    float    _pvW = 0;         uint32_t _pvMs = 0;
    float    _totBmsV = 0;     uint32_t _totBmsVMs = 0;
    float    _totCellV = 0;    uint32_t _totCellMs = 0;
    float    _packCellV[4] = {}; uint32_t _packCellMs[4] = {};
    float    _packV[4] = {};     uint32_t _packVMs[4] = {};
    float    _ccl = 0;         uint32_t _cclMs = 0;
    float    _batTemp = 0;     uint32_t _batTempMs = 0;
    cherokee::Tri     _devGrid = cherokee::Tri::Unknown;    uint32_t _devModeMs = 0;
    cherokee::Tri     _chgUtility = cherokee::Tri::Unknown; uint32_t _chgPrioMs = 0;
    cherokee::OutPrio _outPrio = cherokee::OutPrio::Unknown; uint32_t _outPrioMs = 0;
    cherokee::AcRange _range = cherokee::AcRange::Unknown;  uint32_t _rangeMs = 0;
    float    _invBatI = 0;
    // Home Assistant hint.
    uint32_t _hintMs = 0;   bool _haveHint = false;
    cherokee::Tri     _hintAdvice = cherokee::Tri::Unknown;
    bool     _hintSurplus = false;
    int      _hintOutageMin = -1;
    cherokee::AcRange _hintRange = cherokee::AcRange::Unknown;
    // Sun, cached once a minute.
    uint32_t _sunCalcMs = 0;  bool _sunCalcDone = false;
    bool     _day = false;    int _toSunsetMin = -1;
    cherokee::Inputs _lastIn;

    uint32_t _changes = 0;
};

#endif
