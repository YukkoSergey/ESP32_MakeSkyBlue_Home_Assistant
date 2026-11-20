#ifndef CHEROKEE_H
#define CHEROKEE_H

#include <stdint.h>

// -----------------------------------------------------------------------------
// Two Cherokee car1848tn chargers (~1.2–1.7 kW each) wired straight to the
// battery, past the inverter. Their inputs sit on phases A and B behind a
// two-pole contactor; the contactor coil's neutral runs through relay 3 (D6)
// on its NC contact:
//
//     relay released (board dead, rebooting, pin LOW)  -> chargers ON
//     relay energised                                   -> chargers OFF
//
// So every failure of the board leaves the battery charging. The coil is fed
// from the grid: without grid the chargers are off whatever the board says.
//
// The inverter and the BMS can't see the chargers' current: to the inverter
// they look like solar on the DC bus.
//
// Pure logic, no Arduino: RelayController feeds Inputs about once a second
// and drives the pin; test/cherokee_sim.cpp runs the scenarios on the Mac.
//
// Decision order (first match wins):
//   1. Emergency OFF — cell >= 3.65 V, BMS >= 54.5 V (30 s), BMS blocked
//      charging (CCL 0) below 95 % SOC, battery outside 2..45 °C (1 min).
//      Fresh data only. Back after >= 15 min once everything is in the
//      return band. Three trips in 24 h -> OFF until acknowledged (except
//      SOC < 20 % with everything normal right now).
//   2. Battery voltage emergency / SOC floor -> ON (also cancel manual off).
//   3. Manual off.
//   4. No fresh / frozen SOC -> ON.
//   5. Flapping (> 6 switches an hour) -> ON for an hour.
//   6. Cell cap (cell >= cellCapV for 1 min) -> OFF, battery counts as full.
//   7. Manual on.
//   8. Season logic (see update()); min 15 min on / 10 min off.
// -----------------------------------------------------------------------------
namespace cherokee {

enum class Mode : uint8_t { Auto = 0, On = 1, Off = 2 };
enum class AcRange : uint8_t { Unknown = 0, Apl = 1, Ups = 2 };
enum class OutPrio : uint8_t { Unknown = 0, Utility = 1, Sbu = 2, Solar = 3 };
enum class Tri : uint8_t { Unknown = 0, No = 1, Yes = 2 };

// Per season, SOC %. 0 = not used.
struct Profile {
    uint8_t onSoc;      // ON below this
    uint8_t offSoc;     // OFF at or above this (winter: only with sun or a charging inverter)
    uint8_t floorSoc;   // ON below this whatever else, manual off included
    uint8_t sunsetSoc;  // goal before sunset (winter, shoulder)
};

struct Config {
    Profile p[3];          // summer, shoulder, winter (Season order)
    uint8_t adviceOnSoc;   // shoulder: ON threshold when HA says the sun won't do
    float   cellCapV;      // cell cap; 0 = off
};

Config defaults();
// nullptr if usable, otherwise a message (Ukrainian, shown on the web page).
const char* validate(const Config& c);

enum Season : uint8_t { Summer = 0, Shoulder = 1, Winter = 2 };

struct Inputs {
    uint32_t now = 0;              // millis()
    uint8_t  season = Winter;
    bool     seasonCertain = false;  // date known or season set by hand

    bool  socFresh = false, socFrozen = false;
    int   soc = -1;
    bool  voltEmergency = false;

    bool  cellFresh = false;  float cellMaxV = 0;
    bool  bmsVFresh = false;  float bmsV = 0;
    bool  cclFresh = false;   float ccl = 0;
    bool  tempFresh = false;  float tempC = 0;
    bool  invIFresh = false;  float invBatI = 0;   // + = charging
    bool  gridFresh = false;  float gridV = 0;
    bool  pvFresh = false;    float pvW = 0;
    bool  loadFresh = false;  float loadW = 0;
    bool  extFresh = false;   float extDcW = 0;    // BMS minus inverter, + = into the battery
    float batV = 0;                                // for W -> A

    Tri     gridMode = Tri::Unknown;        // device_mode == Grid
    Tri     chargerUtility = Tri::Unknown;  // charger_source_priority allows utility
    OutPrio outPrio = OutPrio::Unknown;
    AcRange range = AcRange::Unknown;

    bool timeKnown = false;
    bool day = false;          // sun above 5 deg
    int  toSunsetMin = -1;     // -1 = unknown or already set

    // Home Assistant hints, already filtered by their TTL.
    Tri  adviceOn = Tri::Unknown;
    bool surplus = false;
    int  outageSoonMin = -1;
};

enum EventKind : uint8_t {
    EvEmergOff = 0,     // cause, v1 = value
    EvEmergClear,
    EvEmergLock,        // third trip in 24 h
    EvLockLowSoc,       // lockout overridden: SOC < 20 %, v1 = soc
    EvFloorOn,          // v1 = soc, v2 = floor
    EvVoltOn,
    EvFlapLock,         // v1 = switches in the last hour
    EvFlapEnd,
    EvInvNotCharging,   // v1 = max inverter current seen
};
enum Cause : uint8_t { CauseCell = 0, CauseBmsV, CauseCcl, CauseTemp, kCauseCount };

struct Event {
    uint16_t seq;
    uint8_t  kind;
    uint8_t  cause;
    float    v1, v2;
};

class Controller {
public:
    static constexpr uint32_t kMin = 60UL * 1000UL;
    static constexpr uint32_t kMinOnMs        = 15 * kMin;
    static constexpr uint32_t kMinOffMs       = 10 * kMin;
    static constexpr int      kFlapMax        = 6;          // switches an hour; more = lock ON
    static constexpr uint32_t kFlapLockMs     = 60 * kMin;
    static constexpr uint32_t kEmergReturnMs  = 15 * kMin;
    static constexpr uint32_t kIccOnMs        = 5 * kMin;
    static constexpr uint32_t kIccOffMs       = 1 * kMin;
    static constexpr uint32_t kVerifyMs       = 3 * kMin;
    static constexpr float    kVerifyA        = 5.0f;
    static constexpr uint32_t kIccBlockMs     = 30 * kMin;
    static constexpr uint32_t kCapHoldMs      = 1 * kMin;
    static constexpr float    kFullClearCellV = 3.45f;
    // 'Full' also lets go this far below the OFF threshold, so a day's discharge
    // re-arms the sunset / outage top-ups (shoulder: 90 - 10 = 80 %).
    static constexpr int      kFullClearDrop  = 10;
    static constexpr uint32_t kFloatMs        = 10 * kMin;
    static constexpr float    kFloatA         = 3.0f;
    static constexpr uint32_t kSunMeasureMs   = 3 * kMin;   // chargers off this long before "sun now" means anything
    static constexpr uint32_t kSunEnoughMs    = 15 * kMin;
    static constexpr uint32_t kUpsSettleMs    = 10 * kMin;
    static constexpr uint32_t kBootSettleMs   = 3 * kMin;   // no season-logic OFF this soon after boot
    static constexpr uint32_t kBootOffMs      = 5 * kMin;   // manual off from before a reboot: wait this long for the date

    // Called on every switch and decision change; text is a log line.
    typedef void (*LogFn)(const char* line);
    // Called when the mode or the lockout must be persisted.
    typedef void (*PersistFn)(Mode m, bool lockout);

    void begin(const Config& cfg, Mode storedMode, bool storedLock, uint32_t now,
               LogFn log, PersistFn persist);
    void setConfig(const Config& cfg) { _cfg = cfg; }
    const Config& config() const { return _cfg; }
    void update(const Inputs& in);

    void setMode(Mode m, const char* by);
    Mode mode() const { return _mode; }
    // Acknowledge the emergency lockout. Returns true if there was one.
    bool ack(const char* by);

    bool chargersOn() const      { return _on; }
    const char* reason() const   { return _reason; }
    bool holding() const         { return _holding; }   // wants to switch, min time not up yet
    uint32_t holdLeftS() const   { return _holdLeftS; }
    bool invCanCharge() const    { return _icc; }
    bool emergencyOff() const    { return _eTripped; }
    uint8_t emergencyCause() const { return _eCause; }
    bool lockout() const         { return _lock; }
    bool flapLocked() const      { return _flapLock; }
    bool full() const            { return _full; }
    bool aplHold() const         { return _aplHold; }
    bool verifying() const       { return _verifying; }
    bool sunEnough() const       { return _sunEnough; }
    float sunNowW() const        { return _sunNowW; }   // < 0 = unknown (chargers on)
    int  switchesLastHour(uint32_t now) const;
    int  emergTrips24h(uint32_t now) const;
    uint32_t sinceChangeS(uint32_t now) const { return (now - _changedMs) / 1000; }

    // Events for Telegram / HA, oldest first. seq starts at 1.
    static constexpr int kEvents = 6;
    bool eventAfter(uint16_t seq, Event& out) const;
    uint16_t lastEventSeq() const { return _evSeq; }

    static const char* modeName(Mode m);
    static bool parseMode(const char* s, Mode& out);
    static const char* causeName(uint8_t c);

private:
    void switchTo(bool on, const char* reason, uint32_t now, bool counted);
    void event(uint8_t kind, uint8_t cause, float v1, float v2);
    void logf(const char* fmt, ...);
    void updateEmergency(const Inputs& in);
    void updateIcc(const Inputs& in);
    void updateAplHold(const Inputs& in);
    void updateSun(const Inputs& in);
    void updateFloat(const Inputs& in);
    void updateVerify(const Inputs& in);
    bool emergencyClear(const Inputs& in) const;
    bool sunNow(const Inputs& in) const;

    Config    _cfg{};
    LogFn     _log = nullptr;
    PersistFn _persist = nullptr;

    Mode     _mode = Mode::Auto;
    bool     _bootOffPending = false;
    uint32_t _bootMs = 0;

    bool        _on = true;            // relay released at boot = chargers on
    uint32_t    _changedMs = 0;
    const char* _reason = "boot";
    const char* _offWhy = "";          // why auto switched off (resume rules depend on it)
    bool        _decided = false;      // first season-logic decision after boot taken
    bool        _holding = false;
    uint32_t    _holdLeftS = 0;

    // Emergency OFF.
    uint32_t _eSince[kCauseCount] = {};
    bool     _eHas[kCauseCount] = {};
    bool     _eTripped = false;
    uint8_t  _eCause = 0;
    uint32_t _eTripMs = 0;
    uint32_t _eTrips[3] = {};
    uint8_t  _eTripN = 0;
    bool     _lock = false;
    bool     _lockLowSocSent = false;

    // Cell cap.
    uint32_t _capSince = 0;  bool _capHas = false;
    bool     _full = false;

    // Flapping.
    uint32_t _sw[8] = {};
    uint8_t  _swN = 0;
    bool     _flapLock = false;
    uint32_t _flapSince = 0;

    // Inverter can charge from the grid.
    bool     _icc = false;
    bool     _gridHigh = false;
    bool     _iccCandPrev = false;
    uint32_t _iccCandSince = 0;
    bool     _iccBlocked = false;
    uint32_t _iccBlockSince = 0;

    // Closed-loop check after "the inverter will finish it".
    bool     _verifying = false;
    uint32_t _verifyStart = 0;
    float    _verifyMaxA = 0;
    bool     _verifyFailed = false;

    // APL hold (SA lets weak grid through to the house).
    bool     _aplHold = false;
    uint32_t _upsSince = 0;  bool _upsHas = false;
    int      _upsSoc = 0;

    // Sun.
    float    _sunNowW = -1;
    uint32_t _sunOkSince = 0;  bool _sunOkHas = false;
    bool     _sunEnough = false;

    // Float (chargers holding the battery at 100 %).
    uint32_t _floatSince = 0;  bool _floatHas = false;
    bool     _floatDone = false;

    Event    _ev[kEvents] = {};
    uint16_t _evSeq = 0;

    bool     _voltOnSent = false, _floorOnSent = false;
};

}  // namespace cherokee

#endif
