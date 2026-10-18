#include "settings.h"
#include "app_log.h"
#include "compat.h"

// Bump when the Settings layout changes; a mismatching blob falls back to
// defaults instead of being misread.
static constexpr uint32_t kSettingsVersion = 1;

namespace settings {

Settings defaults() {
    Settings s;
    s.capacityKwh = 9.6;   // 1x US5000 + 2x US2000

    // From Solar Assistant history (Nov 2024 – Oct 2026). Off-grid economy
    // load at night: winter p50 150 W / p90 390 W, summer p50 135 W /
    // p90 385 W. Generator via the LiFePO4 charger adds ~0.7 kWh per hour
    // of running. Thresholds are energy, so they hold as batteries are added.
    //
    // Summer: the grid is off on purpose (yearly timer relay) and a battery
    // voltage relay forces it back on when needed, so the generator is only
    // an emergency backstop.
    s.profiles[(int)Season::Summer]   = { 6.0, 4.0, true, 1.5, 3.5 };
    s.profiles[(int)Season::Shoulder] = { 7.0, 5.0, true, 2.5, 5.5 };
    // Winter: heavy loads stop with enough left for a long night in economy
    // mode (14 h x 390 W ~ 5.5 kWh + 1 kWh margin); 3.0 -> 6.5 kWh is ~5 h
    // of generator.
    s.profiles[(int)Season::Winter]   = { 8.2, 6.5, true, 3.0, 6.5 };

    s.seasonMode = SeasonMode::Auto;
    return s;
}

Settings load() {
    Preferences p;
    p.begin("settings", true);
    Settings s;
    bool ok = p.getUInt("ver", 0) == kSettingsVersion
           && p.getBytesLength("blob") == sizeof(Settings)
           && p.getBytes("blob", &s, sizeof(Settings)) == sizeof(Settings)
           && validate(s).isEmpty();
    p.end();
    if (!ok) {
        Log.println("[Settings] no valid stored settings, using defaults");
        return defaults();
    }
    return s;
}

void save(const Settings& s) {
    Preferences p;
    p.begin("settings", false);
    p.putBytes("blob", &s, sizeof(Settings));
    p.putUInt("ver", kSettingsVersion);
    p.end();
    Log.println("[Settings] saved");
}

String validate(const Settings& s) {
    if (!(s.capacityKwh > 0 && s.capacityKwh <= 200)) return "capacity must be 0..200 kWh";
    if ((uint8_t)s.seasonMode > (uint8_t)SeasonMode::Winter) return "bad season mode";
    for (int i = 0; i < kSeasonCount; i++) {
        const SeasonProfile& p = s.profiles[i];
        String name = seasonName((Season)i);
        if (!(p.heavyOffKwh >= 0 && p.heavyOnKwh <= s.capacityKwh))
            return name + ": heavy-load thresholds must be within 0..capacity";
        if (!(p.heavyOnKwh > p.heavyOffKwh))
            return name + ": heavy-load ON must be above OFF";
        if (!(p.genStartKwh >= 0 && p.genStopKwh > 0 && p.genStopKwh <= s.capacityKwh))
            return name + ": generator thresholds must be within 0..capacity";
        if (p.genAuto && !(p.genStopKwh > p.genStartKwh))
            return name + ": generator STOP must be above START";
    }
    return "";
}

const char* seasonName(Season s) {
    switch (s) {
        case Season::Summer:   return "summer";
        case Season::Shoulder: return "shoulder";
        case Season::Winter:   return "winter";
    }
    return "?";
}

const char* seasonModeName(SeasonMode m) {
    switch (m) {
        case SeasonMode::Auto:     return "auto";
        case SeasonMode::Summer:   return "summer";
        case SeasonMode::Shoulder: return "shoulder";
        case SeasonMode::Winter:   return "winter";
    }
    return "?";
}

Season seasonForDate(int month, int day) {
    if (month == 12 || month <= 2) return Season::Winter;
    if (month <= 4)                return Season::Shoulder;
    if (month < 10 || (month == 10 && day < 15)) return Season::Summer;
    return Season::Shoulder;
}

}  // namespace settings
