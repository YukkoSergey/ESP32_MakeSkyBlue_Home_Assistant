#ifndef SETTINGS_H
#define SETTINGS_H

#include <Arduino.h>

// -----------------------------------------------------------------------------
// User-tunable relay settings, persisted in NVS and editable from the web UI.
//
// Thresholds are in kWh of stored energy, not SOC %, so they keep their meaning
// as batteries are added — only capacityKwh changes. Reserve is computed as
// SOC% * capacityKwh / 100.
// -----------------------------------------------------------------------------
enum class Season : uint8_t { Summer = 0, Shoulder = 1, Winter = 2 };
constexpr int kSeasonCount = 3;

enum class SeasonMode : uint8_t { Auto = 0, Summer = 1, Shoulder = 2, Winter = 3 };

struct SeasonProfile {
    float heavyOnKwh;    // heavy load allowed once reserve rises above this
    float heavyOffKwh;   // ...and cut once it drops below this
    bool  genAuto;       // false = never start the generator automatically
    float genStartKwh;   // auto start below this
    float genStopKwh;    // stop above this (also the goal of a manual run)
};

struct Settings {
    float         capacityKwh;
    SeasonProfile profiles[kSeasonCount];
    SeasonMode    seasonMode;
};

namespace settings {
    // Defaults are a starting point, not a recommendation — tune in the web UI.
    Settings defaults();
    Settings load();
    void     save(const Settings& s);
    // Returns an error message, or empty string if the settings are usable.
    String   validate(const Settings& s);

    const char* seasonName(Season s);
    const char* seasonModeName(SeasonMode m);

    // Calendar season for a local date (month 1..12, day 1..31):
    //   winter   Dec 1  – Feb end
    //   shoulder Mar 1  – Apr 30, Oct 15 – Nov 30
    //   summer   May 1  – Oct 14
    Season seasonForDate(int month, int day);
}

#endif
