#include "cherokee.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

// Log formats live in flash on the ESP8266 (string literals are RAM there).
#if defined(ESP8266)
#include <pgmspace.h>
#define CHF(s) PSTR(s)
#define CH_VSNPRINTF vsnprintf_P
#else
#define CHF(s) (s)
#define CH_VSNPRINTF vsnprintf
#endif

namespace cherokee {

static constexpr float kGridPresentV = 100.0f;
static constexpr float kWeakGridV    = 170.0f;   // SA's Automation #5: APL at or below this

// Emergency OFF thresholds (see the spec table; history 08–18.11: full
// charges end at cell 3.55–3.61 V, BMS max 53.5 V, temperature 16–39.5 °C,
// CCL 0 only at SOC 98–100 %).
static constexpr float    kCellTripV   = 3.65f, kCellBackV = 3.50f;
static constexpr float    kBmsTripV    = 54.5f, kBmsBackV  = 53.5f;
static constexpr int      kCclSocBelow = 95;
static constexpr float    kTempHighC   = 45.0f, kTempLowC  = 2.0f;
static constexpr float    kTempBackHi  = 40.0f, kTempBackLo = 5.0f;
static constexpr uint32_t kVoltHoldMs  = 30UL * 1000UL;
static constexpr uint32_t kSlowHoldMs  = 60UL * 1000UL;
static constexpr uint32_t kDayMs       = 24UL * 60UL * 60UL * 1000UL;
static constexpr int      kLockLowSoc  = 20;

Config defaults() {
    Config c{};
    c.p[Summer]   = {30, 50, 30, 0};
    c.p[Shoulder] = {60, 90, 40, 90};
    c.p[Winter]   = {95, 97, 0, 100};
    c.adviceOnSoc = 85;
    c.cellCapV    = 3.55f;
    return c;
}

const char* validate(const Config& c) {
    for (int i = 0; i < 3; i++) {
        const Profile& p = c.p[i];
        if (p.onSoc > 100 || p.offSoc > 100 || p.floorSoc > 100 || p.sunsetSoc > 100)
            return "пороги Cherokee мають бути 0..100 %";
        if (p.offSoc <= p.onSoc) return "поріг ВИМК Cherokee має бути вищим за ВВІМК";
        if (p.floorSoc > p.onSoc && p.onSoc) return "підлога Cherokee не може бути вищою за поріг ВВІМК";
    }
    if (c.adviceOnSoc > 100) return "поріг за порадою HA має бути 0..100 %";
    if (!(c.cellCapV == 0 || (c.cellCapV >= 3.40f && c.cellCapV <= 3.65f)))
        return "стеля комірки: 0 (вимкнено) або 3.40..3.65 В";
    return nullptr;
}

// Condition held continuously for `ms`.
static bool held(bool cond, uint32_t now, uint32_t& since, bool& has, uint32_t ms) {
    if (!cond) { has = false; return false; }
    if (!has) { has = true; since = now; }
    return now - since >= ms;
}

void Controller::logf(const char* fmt, ...) {
    if (!_log) return;
    char buf[200];
    va_list ap;
    va_start(ap, fmt);
    CH_VSNPRINTF(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    _log(buf);
}

void Controller::event(uint8_t kind, uint8_t cause, float v1, float v2) {
    _evSeq++;
    _ev[(_evSeq - 1) % kEvents] = {_evSeq, kind, cause, v1, v2};
}

bool Controller::eventAfter(uint16_t seq, Event& out) const {
    if (seq >= _evSeq) return false;
    uint16_t next = seq + 1;
    if (_evSeq - next >= kEvents) next = _evSeq - kEvents + 1;
    out = _ev[(next - 1) % kEvents];
    return true;
}

void Controller::begin(const Config& cfg, Mode storedMode, bool storedLock, uint32_t now,
                       LogFn log, PersistFn persist) {
    _cfg = cfg;
    _log = log;
    _persist = persist;
    _bootMs = now;
    _changedMs = now;
    _on = true;   // the relay starts released: chargers on
    _lock = storedLock;
    // Manual "off" survives a reboot only in summer; the season is known
    // once the date is, so decide that in update(). "On" falls back to auto.
    _mode = storedMode == Mode::Off ? Mode::Off : Mode::Auto;
    _bootOffPending = _mode == Mode::Off;
    if (storedMode == Mode::On && _persist) _persist(_mode, _lock);
    logf(CHF("[Cherokee] start: chargers ON (relay released), mode %s%s"), modeName(_mode),
         _lock ? ", EMERGENCY LOCKOUT kept (needs acknowledgement)" : "");
}

void Controller::setMode(Mode m, const char* by) {
    _bootOffPending = false;
    if (m == _mode) return;
    logf(CHF("[Cherokee] mode %s -> %s (by %s)"), modeName(_mode), modeName(m), by);
    _mode = m;
    if (_persist) _persist(_mode, _lock);
}

bool Controller::ack(const char* by) {
    if (!_lock) return false;
    _lock = false;
    _eTripN = 0;
    _lockLowSocSent = false;
    logf(CHF("[Cherokee] emergency lockout acknowledged (by %s)"), by);
    if (_persist) _persist(_mode, _lock);
    return true;
}

int Controller::switchesLastHour(uint32_t now) const {
    int n = 0;
    for (int i = 0; i < _swN; i++)
        if (now - _sw[i] < 60UL * kMin) n++;
    return n;
}

int Controller::emergTrips24h(uint32_t now) const {
    int n = 0;
    for (int i = 0; i < _eTripN; i++)
        if (now - _eTrips[i] < kDayMs) n++;
    return n;
}

void Controller::switchTo(bool on, const char* reason, uint32_t now, bool counted) {
    _reason = reason;
    if (on == _on) return;
    _on = on;
    _changedMs = now;
    if (on) {
        _verifying = false;
        _verifyFailed = false;
    } else {
        _offWhy = reason;
    }
    _floatHas = false;
    _floatDone = false;
    _sunOkHas = false;
    _sunEnough = false;
    logf(CHF("[Cherokee] chargers -> %s (%s)"), on ? "ON" : "OFF", reason);
    if (!counted) return;
    // Sliding window of the last switches.
    if (_swN == sizeof(_sw) / sizeof(_sw[0])) {
        memmove(_sw, _sw + 1, sizeof(_sw) - sizeof(_sw[0]));
        _swN--;
    }
    _sw[_swN++] = now;
    int n = switchesLastHour(now);
    if (n > kFlapMax && !_flapLock) {
        _flapLock = true;
        _flapSince = now;
        logf(CHF("[Cherokee] %d switches in an hour -> locked ON for 60 min"), n);
        event(EvFlapLock, 0, (float)n, 0);
    }
}

bool Controller::emergencyClear(const Inputs& in) const {
    if (in.cellFresh && in.cellMaxV > kCellBackV) return false;
    if (in.bmsVFresh && in.bmsV > kBmsBackV) return false;
    if (in.cclFresh && in.ccl <= 0.05f) return false;
    if (in.tempFresh && (in.tempC > kTempBackHi || in.tempC < kTempBackLo)) return false;
    return true;
}

void Controller::updateEmergency(const Inputs& in) {
    const uint32_t now = in.now;
    bool c[kCauseCount];
    float v[kCauseCount] = {in.cellMaxV, in.bmsV, in.ccl, in.tempC};
    c[CauseCell] = held(in.cellFresh && in.cellMaxV >= kCellTripV, now, _eSince[CauseCell], _eHas[CauseCell], kVoltHoldMs);
    c[CauseBmsV] = held(in.bmsVFresh && in.bmsV >= kBmsTripV, now, _eSince[CauseBmsV], _eHas[CauseBmsV], kVoltHoldMs);
    c[CauseCcl]  = held(in.cclFresh && in.ccl <= 0.05f && in.socFresh && !in.socFrozen && in.soc < kCclSocBelow,
                        now, _eSince[CauseCcl], _eHas[CauseCcl], kSlowHoldMs);
    c[CauseTemp] = held(in.tempFresh && (in.tempC >= kTempHighC || in.tempC <= kTempLowC),
                        now, _eSince[CauseTemp], _eHas[CauseTemp], kSlowHoldMs);
    int cause = -1;
    for (int i = 0; i < kCauseCount; i++)
        if (c[i]) { cause = i; break; }

    if (!_eTripped && cause >= 0) {
        _eTripped = true;
        _eCause = (uint8_t)cause;
        _eTripMs = now;
        if (_eTripN == 3) { memmove(_eTrips, _eTrips + 1, sizeof(_eTrips) - sizeof(_eTrips[0])); _eTripN--; }
        _eTrips[_eTripN++] = now;
        logf(CHF("[Cherokee] EMERGENCY OFF: %s = %.3f"), causeName(cause), v[cause]);
        event(EvEmergOff, (uint8_t)cause, v[cause], in.socFresh ? (float)in.soc : -1.0f);
        if (emergTrips24h(now) >= 3 && !_lock) {
            _lock = true;
            logf(CHF("[Cherokee] 3 emergency trips in 24 h -> OFF until acknowledged"));
            event(EvEmergLock, (uint8_t)cause, 3, 0);
            if (_persist) _persist(_mode, _lock);
        }
    } else if (_eTripped && cause < 0 && now - _eTripMs >= kEmergReturnMs && emergencyClear(in)) {
        _eTripped = false;
        logf(CHF("[Cherokee] emergency cleared after %lu min"), (unsigned long)((now - _eTripMs) / kMin));
        event(EvEmergClear, _eCause, 0, 0);
    }
}

void Controller::updateIcc(const Inputs& in) {
    const uint32_t now = in.now;
    float onV = 0, offV = 0;
    if (in.range == AcRange::Apl)      { onV = 160; offV = 152; }
    else if (in.range == AcRange::Ups) { onV = 180; offV = 172; }
    if (!in.gridFresh || in.range == AcRange::Unknown) _gridHigh = false;
    else if (in.gridV >= onV)  _gridHigh = true;
    else if (in.gridV < offV)  _gridHigh = false;

    if (_iccBlocked && now - _iccBlockSince >= kIccBlockMs) {
        _iccBlocked = false;
        logf(CHF("[Cherokee] inverter grid charging trusted again (30 min block over)"));
    }
    bool cand = in.gridMode == Tri::Yes && in.chargerUtility == Tri::Yes &&
                in.outPrio == OutPrio::Utility && _gridHigh && !_iccBlocked;
    if (cand != _iccCandPrev) { _iccCandPrev = cand; _iccCandSince = now; }
    if (_iccBlocked) cand = false;
    if (cand != _icc && (_iccBlocked || now - _iccCandSince >= (cand ? kIccOnMs : kIccOffMs))) {
        _icc = cand;
        logf(CHF("[Cherokee] inverter can charge from the grid: %s (grid %.0f V, %s)"), _icc ? "yes" : "no",
             in.gridV, in.range == AcRange::Apl ? "APL" : in.range == AcRange::Ups ? "UPS" : "range ?");
    }
}

void Controller::updateVerify(const Inputs& in) {
    if (!_verifying) return;
    if (_on) { _verifying = false; return; }
    if (in.invIFresh && in.invBatI > _verifyMaxA) _verifyMaxA = in.invBatI;
    if (_verifyMaxA > kVerifyA) {
        _verifying = false;
        logf(CHF("[Cherokee] check passed: inverter charges the battery (%.1f A)"), _verifyMaxA);
        return;
    }
    if (in.now - _verifyStart >= kVerifyMs) {
        _verifying = false;
        _verifyFailed = true;
        _iccBlocked = true;
        _iccBlockSince = in.now;
        _icc = false;
        logf(CHF("[Cherokee] check FAILED: inverter not charging (max %.1f A in 3 min) -> grid charging not trusted for 30 min"),
             _verifyMaxA);
        event(EvInvNotCharging, 0, _verifyMaxA, 0);
    }
}

void Controller::updateAplHold(const Inputs& in) {
    bool weak = in.season != Summer && in.range == AcRange::Apl && in.gridFresh &&
                in.gridV >= kGridPresentV && in.gridV < kWeakGridV;
    if (weak) {
        if (!_aplHold) logf(CHF("[Cherokee] SA has APL with weak grid (%.0f V): chargers on until UPS"), in.gridV);
        _aplHold = true;
        _upsHas = false;
        return;
    }
    if (!_aplHold) return;
    if (in.season == Summer || in.range == AcRange::Unknown) {
        _aplHold = false;
        return;
    }
    if (in.range != AcRange::Ups) { _upsHas = false; return; }
    if (!_upsHas) { _upsHas = true; _upsSince = in.now; _upsSoc = in.soc; }
    if (in.now - _upsSince >= kUpsSettleMs && in.socFresh && (in.soc >= _upsSoc + 2 || in.soc >= 100)) {
        _aplHold = false;
        logf(CHF("[Cherokee] UPS for 10 min and SOC %d %%: APL hold over"), in.soc);
    }
}

void Controller::updateSun(const Inputs& in) {
    bool measurable = !_on && in.now - _changedMs >= kSunMeasureMs && in.pvFresh && in.extFresh;
    _sunNowW = measurable ? in.pvW + (in.extDcW > 0 ? in.extDcW : 0) : -1;
    bool ok = measurable && in.loadFresh && _sunNowW >= in.loadW + 300 && in.toSunsetMin > 120;
    _sunEnough = held(ok, in.now, _sunOkSince, _sunOkHas, kSunEnoughMs);
}

void Controller::updateFloat(const Inputs& in) {
    bool c = _on && in.socFresh && !in.socFrozen && in.soc >= 100 && in.extFresh && in.batV > 40 &&
             in.extDcW / in.batV < kFloatA;
    _floatDone = held(c, in.now, _floatSince, _floatHas, kFloatMs);
}

bool Controller::sunNow(const Inputs& in) const {
    return in.day && ((in.pvFresh && in.pvW >= 150) || in.surplus);
}

void Controller::update(const Inputs& in) {
    const uint32_t now = in.now;

    if (_bootOffPending && (in.seasonCertain || now - _bootMs >= kBootOffMs)) {
        _bootOffPending = false;
        if (in.season != Summer && _mode == Mode::Off) {
            _mode = Mode::Auto;
            logf(CHF("[Cherokee] manual off from before the reboot dropped (not summer) -> auto"));
            if (_persist) _persist(_mode, _lock);
        }
    }

    updateEmergency(in);
    updateIcc(in);
    updateVerify(in);
    updateAplHold(in);
    updateSun(in);
    updateFloat(in);

    const Profile& P = _cfg.p[in.season < 3 ? in.season : Winter];
    const bool trusted = in.socFresh && !in.socFrozen;

    bool capOn = _cfg.cellCapV > 0 && in.cellFresh && in.cellMaxV >= _cfg.cellCapV;
    bool cap = held(capOn, now, _capSince, _capHas, kCapHoldMs);
    if (cap && !_full) {
        _full = true;
        logf(CHF("[Cherokee] cell %.3f V >= cap %.2f V: battery counts as full"), in.cellMaxV, _cfg.cellCapV);
    }
    if (_full && (_cfg.cellCapV <= 0 ||
                  (trusted && in.soc <= P.onSoc && (!in.cellFresh || in.cellMaxV <= kFullClearCellV)))) {
        _full = false;
        logf(CHF("[Cherokee] 'full after cell cap' cleared (SOC %d %%)"), in.soc);
    }

    if (_flapLock && now - _flapSince >= kFlapLockMs) {
        _flapLock = false;
        _swN = 0;
        logf(CHF("[Cherokee] flapping lock over"));
        event(EvFlapEnd, 0, 0, 0);
    }

    _holding = false;
    _holdLeftS = 0;

    // 1. Emergency OFF — above everything, manual "on" included.
    if (_eTripped) {
        static const char* names[kCauseCount] = {"emerg_cell", "emerg_bms_v", "emerg_ccl", "emerg_temp"};
        switchTo(false, names[_eCause], now, true);
        return;
    }
    if (_lock) {
        bool low = trusted && in.soc < kLockLowSoc && emergencyClear(in);
        if (low) {
            if (!_lockLowSocSent) {
                _lockLowSocSent = true;
                event(EvLockLowSoc, 0, (float)in.soc, 0);
            }
            switchTo(true, "emerg_lock_low_soc", now, false);
        } else {
            if (trusted && in.soc >= kLockLowSoc + 5) _lockLowSocSent = false;
            switchTo(false, "emerg_lock", now, false);
        }
        return;
    }

    // 2. Battery protection: ON in every mode, manual off cancelled.
    bool floor = trusted && P.floorSoc && in.soc < P.floorSoc;
    if (in.voltEmergency || floor) {
        if (_mode == Mode::Off) {
            _mode = Mode::Auto;
            logf(CHF("[Cherokee] manual off cancelled: %s"), in.voltEmergency ? "battery voltage emergency" : "SOC below floor");
            if (_persist) _persist(_mode, _lock);
        }
        if (in.voltEmergency && !_voltOnSent) { _voltOnSent = true; event(EvVoltOn, 0, 0, 0); }
        if (!in.voltEmergency && floor && !_floorOnSent) {
            _floorOnSent = true;
            event(EvFloorOn, 0, (float)in.soc, (float)P.floorSoc);
        }
        switchTo(true, in.voltEmergency ? "volt_emerg" : "soc_floor", now, true);
        return;
    }
    if (!in.voltEmergency) _voltOnSent = false;
    if (trusted && in.soc >= P.floorSoc + 2) _floorOnSent = false;

    // 3. Manual off.
    if (_mode == Mode::Off) { switchTo(false, "manual_off", now, false); return; }

    // 4. No data we can trust -> charge.
    if (!in.socFresh) { switchTo(true, "no_data", now, true); return; }
    if (in.socFrozen) { switchTo(true, "frozen", now, true); return; }

    // 5. Flapping.
    if (_flapLock) { switchTo(true, "flap_lock", now, true); return; }

    // 6. Cell cap: not an emergency, but beats min-on time and manual on.
    if (cap) {
        if (_on && now - _bootMs < kBootSettleMs) {   // see kBootSettleMs; emergency OFF still acts
            _holding = true;
            _holdLeftS = (kBootSettleMs - (now - _bootMs)) / 1000;
            _reason = "cell_cap";
            return;
        }
        switchTo(false, "cell_cap", now, true);
        return;
    }

    // 7. Manual on.
    if (_mode == Mode::On) { switchTo(true, "manual_on", now, false); return; }

    // 8. Season logic.
    const int soc = in.soc;
    int onSoc = P.onSoc;
    if (in.season == Shoulder && in.adviceOn == Tri::Yes && _cfg.adviceOnSoc > onSoc) onSoc = _cfg.adviceOnSoc;
    const bool notSummer = in.season != Summer;
    const bool sun = sunNow(in);

    const char* trig = nullptr;
    if (soc < onSoc) trig = "soc_low";
    else if (notSummer && _aplHold) trig = "apl_weak_grid";
    else if (notSummer && P.sunsetSoc && in.toSunsetMin >= 0 && in.toSunsetMin <= 90 && soc < P.sunsetSoc && !_full)
        trig = "sunset";
    else if (in.outageSoonMin >= 0 && in.outageSoonMin <= 120 && soc < 100 && !_full) trig = "outage_soon";
    // Shoulder only: in winter the same state is rule "to 100 %" below. A
    // 5-point margin when off, so it doesn't nibble at the OFF threshold.
    else if (in.season == Shoulder && in.range == AcRange::Ups && in.gridFresh && in.gridV >= kGridPresentV &&
             in.gridV < kWeakGridV && !_sunEnough && !_full && soc < P.offSoc - (_on ? 0 : 5))
        trig = "ups_weak_grid";

    // After a reboot the relay is released (chargers on) by wiring, not by a
    // decision: the first one is taken from scratch, as if they were off,
    // and isn't held by the minimum on-time.
    const bool fresh = !_decided;
    const bool on = _on && _decided;

    bool want;
    const char* why;
    bool urgent = fresh;
    if (trig) {
        want = true;
        why = trig;
    } else if (on) {
        want = false;
        if (_full) why = "full";
        else if (in.season != Winter) {
            if (soc >= P.offSoc) why = "soc_high";
            else { want = true; why = "charging"; }
        }
        else if (soc >= P.offSoc && sun) why = "sun";
        else if (soc >= P.offSoc && _icc) why = "inverter";
        else if (soc >= 100 && _floatDone) why = "float";
        else { want = true; why = _iccBlocked ? "inv_not_charging" : soc >= P.offSoc ? "to_full" : "charging"; }
    } else {
        want = false;
        why = *_offWhy ? _offWhy : "idle";
        if (in.season == Winter && !_full) {
            bool afterFull = !strcmp(_offWhy, "float") || !strcmp(_offWhy, "full") || !strcmp(_offWhy, "cell_cap");
            if (afterFull) {
                if (soc <= P.onSoc) { want = true; why = "soc_low"; }
            } else if (_verifyFailed) {
                want = true; why = "inv_not_charging"; urgent = true;
            } else if (soc < P.offSoc && !sun && !_icc) {
                want = true;
                why = !strcmp(_offWhy, "inverter") ? "inv_lost" : !strcmp(_offWhy, "sun") ? "sun_gone" : "to_full";
            } else if (!strcmp(_offWhy, "sun") && !sun && _icc) {
                // The sun is gone and the inverter is to finish the job: check it does.
                _offWhy = "inverter";
                why = "inverter";
                _verifying = true;
                _verifyStart = now;
                _verifyMaxA = 0;
                logf(CHF("[Cherokee] sun gone, inverter charges from the grid: checking it does"));
            }
        }
    }

    if (want == _on) { _reason = why; _decided = true; return; }
    // A board in a crash loop must not click the contactor every minute:
    // the first minutes after boot the chargers stay as wired (on). Only
    // the protections above (emergency, cell cap, manual) act sooner.
    if (!want && now - _bootMs < kBootSettleMs) {
        _holding = true;
        _holdLeftS = (kBootSettleMs - (now - _bootMs)) / 1000;
        _reason = why;
        return;   // still undecided: the first real decision comes after the settle time
    }
    _decided = true;
    uint32_t el = now - _changedMs;
    uint32_t minMs = _on ? kMinOnMs : kMinOffMs;
    if (!urgent && el < minMs) {
        _holding = true;
        _holdLeftS = (minMs - el) / 1000;
        _reason = why;
        return;
    }
    switchTo(want, why, now, true);
    if (!want && !strcmp(why, "inverter")) {
        _verifying = true;
        _verifyStart = now;
        _verifyMaxA = 0;
    }
}

const char* Controller::modeName(Mode m) {
    switch (m) {
        case Mode::Auto: return "auto";
        case Mode::On:   return "on";
        case Mode::Off:  return "off";
    }
    return "?";
}

bool Controller::parseMode(const char* s, Mode& out) {
    if (!strcmp(s, "auto")) { out = Mode::Auto; return true; }
    if (!strcmp(s, "on"))   { out = Mode::On;   return true; }
    if (!strcmp(s, "off"))  { out = Mode::Off;  return true; }
    return false;
}

const char* Controller::causeName(uint8_t c) {
    switch (c) {
        case CauseCell: return "cell";
        case CauseBmsV: return "bms_v";
        case CauseCcl:  return "ccl";
        case CauseTemp: return "temp";
    }
    return "?";
}

}  // namespace cherokee
