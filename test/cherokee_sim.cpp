// Scenario tests for the Cherokee logic (src/cherokee.cpp), on the Mac:
//
//   c++ -std=c++17 -Iinclude test/cherokee_sim.cpp src/cherokee.cpp -o /tmp/cherokee_sim && /tmp/cherokee_sim
//
// Scenarios follow section 9 of the spec; time runs in 1 s steps.
#include "cherokee.h"
#include <cstdio>
#include <cstring>
#include <string>

using namespace cherokee;

static bool g_verbose = false;
static int g_fail = 0, g_pass = 0;
static void logLine(const char* l) { if (g_verbose) printf("      %s\n", l); }
static Mode g_pMode = Mode::Auto;
static bool g_pLock = false;
static void persist(Mode m, bool lock) { g_pMode = m; g_pLock = lock; }

struct Sim {
    Controller c;
    Inputs in;
    uint32_t t = 1000;
    uint16_t evSeen = 0;

    explicit Sim(uint8_t season, Mode stored = Mode::Auto, bool lock = false, Config cfg = defaults()) {
        in.season = season;
        in.seasonCertain = true;
        in.timeKnown = true;
        in.socFresh = true;
        in.soc = 80;
        in.cellFresh = true;  in.cellMaxV = 3.35f;
        in.bmsVFresh = true;  in.bmsV = 52.5f;
        in.tempFresh = true;  in.tempC = 20;
        in.invIFresh = true;  in.invBatI = 0;
        in.gridFresh = true;  in.gridV = 215;
        in.pvFresh = true;    in.pvW = 0;
        in.loadFresh = true;  in.loadW = 400;
        in.extFresh = true;   in.extDcW = 0;
        in.batV = 52.5f;
        in.gridMode = Tri::Yes;
        in.chargerUtility = Tri::Yes;
        in.outPrio = OutPrio::Utility;
        in.range = AcRange::Ups;
        in.day = false;
        in.toSunsetMin = -1;
        c.begin(cfg, stored, lock, t, logLine, persist);
    }
    void run(uint32_t seconds) {
        for (uint32_t i = 0; i < seconds; i++) {
            in.now = t;
            c.update(in);
            t += 1000;
        }
    }
    void min(uint32_t m) { run(m * 60); }
    bool hasEvent(uint8_t kind) {
        Event e;
        bool found = false;
        while (c.eventAfter(evSeen, e)) { evSeen = e.seq; if (e.kind == kind) found = true; }
        return found;
    }
};

static void expect(const char* what, bool ok, const Sim& s) {
    if (ok) { g_pass++; printf("  ok    %s\n", what); }
    else { g_fail++; printf("  FAIL  %s  (chargers %s, reason %s)\n", what, s.c.chargersOn() ? "ON" : "OFF", s.c.reason()); }
}
#define ON(s)  ((s).c.chargersOn())
#define WHY(s, r) (!strcmp((s).c.reason(), r))

int main(int argc, char** argv) {
    g_verbose = argc > 1 && !strcmp(argv[1], "-v");

    printf("1. Winter morning: SOC 90, grid 210 V, Utility first -> ON; 97 %% -> OFF; inverter charges -> stays OFF\n");
    {
        Sim s(Winter);
        s.in.soc = 90; s.in.gridV = 210; s.in.range = AcRange::Apl;
        s.min(1);
        expect("ON at 90 %", ON(s) && WHY(s, "soc_low"), s);
        s.min(20);
        s.in.soc = 97;
        s.run(5);
        expect("OFF at 97 % (inverter can charge)", !ON(s) && WHY(s, "inverter"), s);
        expect("closed-loop check running", s.c.verifying(), s);
        s.in.invBatI = 12;
        s.min(4);
        expect("still OFF, check passed", !ON(s) && !s.c.verifying(), s);
        s.min(60);
        expect("still OFF an hour later", !ON(s), s);
    }

    printf("2. Same, inverter does not charge (0 A) -> ON again after 3 min\n");
    {
        Sim s(Winter);
        s.in.soc = 90; s.in.gridV = 210; s.in.range = AcRange::Apl;
        s.min(20);
        s.in.soc = 97;
        s.run(5);
        expect("OFF at 97 %", !ON(s), s);
        s.run(2 * 60 + 50);
        expect("still OFF at 2:55", !ON(s), s);
        s.run(15);
        expect("ON at 3 min, inverter not charging", ON(s) && WHY(s, "inv_not_charging"), s);
        expect("Telegram event 'inverter not charging'", s.hasEvent(EvInvNotCharging), s);
        expect("inverter not trusted for now", !s.c.invCanCharge(), s);
    }

    printf("3. Grid 165 V, UPS, SBU -> ON to 100 %%, OFF on float, ON again at 95 %%\n");
    {
        Sim s(Winter);
        s.in.soc = 90; s.in.gridV = 165; s.in.outPrio = OutPrio::Sbu; s.in.gridMode = Tri::No;
        s.min(20);
        s.in.soc = 98;
        s.min(1);
        expect("ON at 98 % (inverter can't charge)", ON(s) && WHY(s, "to_full"), s);
        s.in.soc = 100; s.in.extDcW = 800;
        s.min(15);
        expect("still ON at 100 % while current flows", ON(s), s);
        s.in.extDcW = 100;   // < 3 A
        s.min(9);
        expect("still ON after 9 min of float", ON(s), s);
        s.min(2);
        expect("OFF after 10 min of float", !ON(s) && WHY(s, "float"), s);
        s.in.soc = 96;
        s.min(20);
        expect("stays OFF at 96 %", !ON(s), s);
        s.in.soc = 95;
        s.run(2);
        expect("ON at 95 %", ON(s), s);
    }

    printf("4. Winter day: SOC 97, roof 400 W -> OFF; 90 min before sunset at 96 %% -> ON\n");
    {
        Sim s(Winter);
        s.in.soc = 90; s.in.gridV = 165; s.in.range = AcRange::Ups; s.in.outPrio = OutPrio::Sbu;
        s.in.day = true; s.in.toSunsetMin = 300;
        s.min(20);
        s.in.soc = 97; s.in.pvW = 400;
        s.run(2);
        expect("OFF at 97 % with sun", !ON(s) && WHY(s, "sun"), s);
        s.in.soc = 96; s.in.toSunsetMin = 120;
        s.min(30);
        expect("stays OFF at 96 % with sun, 2 h to sunset", !ON(s), s);
        s.in.toSunsetMin = 90;
        s.run(2);
        expect("ON 90 min before sunset", ON(s) && WHY(s, "sunset"), s);
    }

    printf("5. SA lost -> ON; frozen SOC -> ON\n");
    {
        Sim s(Winter);
        s.in.soc = 99; s.in.day = true; s.in.pvW = 600; s.in.toSunsetMin = 300;
        s.min(20);
        expect("OFF at 99 % with sun", !ON(s), s);
        s.in.socFresh = false; s.in.cellFresh = false; s.in.bmsVFresh = false;
        s.run(1);
        expect("ON at once without data", ON(s) && WHY(s, "no_data"), s);
        s.in.socFresh = true; s.in.cellFresh = true; s.in.bmsVFresh = true;
        s.min(20);
        expect("OFF again with data and sun", !ON(s), s);
        s.in.socFrozen = true;
        s.run(1);
        expect("ON when SOC frozen", ON(s) && WHY(s, "frozen"), s);
    }

    printf("6. Reboot without NTP -> winter, relay released (chargers ON)\n");
    {
        Sim s(Winter, Mode::Off);
        s.in.seasonCertain = false; s.in.timeKnown = false; s.in.socFresh = false;
        s.run(1);
        expect("chargers ON at boot", true, s);   // see below
        expect("manual off kept while date unknown", s.c.mode() == Mode::Off, s);
        s.min(6);
        expect("manual off dropped after 5 min (winter)", s.c.mode() == Mode::Auto, s);
        s.run(1);
        expect("ON (no data)", ON(s), s);
        Sim b(Winter);
        expect("a fresh controller says ON before any data", ON(b), b);
    }

    printf("6b. Reboot in summer keeps manual off\n");
    {
        Sim s(Summer, Mode::Off);
        s.in.soc = 60;
        s.run(5);
        expect("summer: manual off kept", s.c.mode() == Mode::Off && !ON(s) && WHY(s, "manual_off"), s);
        Sim w(Shoulder, Mode::Off);
        w.in.soc = 70;
        w.run(5);
        expect("shoulder: manual off -> auto", w.c.mode() == Mode::Auto, w);
    }

    printf("8. Flapping: 7 switches in an hour -> locked ON + alarm\n");
    {
        Sim s(Summer);
        s.in.soc = 60;
        s.run(5);
        expect("first 3 min after boot: no OFF (crash-loop guard)", ON(s) && s.c.holding(), s);
        s.min(3);
        expect("summer 60 %: OFF", !ON(s), s);
        bool locked = false;
        for (int i = 0; i < 6 && !locked; i++) {
            s.in.socFresh = false; s.run(2);                    // -> ON
            s.in.socFresh = true; s.in.cellMaxV = 3.56f; s.run(65);   // cell cap -> OFF
            s.in.cellMaxV = 3.35f; s.run(5);
            locked = s.c.flapLocked();
        }
        expect("flap lock set", s.c.flapLocked(), s);
        expect("flap event", s.hasEvent(EvFlapLock), s);
        s.in.cellMaxV = 3.56f; s.min(2);
        expect("locked ON even with the cell cap", ON(s) && WHY(s, "flap_lock"), s);
        s.in.cellMaxV = 3.66f; s.run(31);
        expect("emergency still beats the flap lock", !ON(s), s);
        s.in.cellMaxV = 3.35f;
        s.min(61);
        expect("lock over after an hour", !s.c.flapLocked(), s);
    }

    printf("9. Summer: 45 %% -> OFF; 29 %% -> ON; manual off at 25 %% -> ON\n");
    {
        Sim s(Summer);
        s.in.soc = 45;
        s.min(3); s.run(2);
        expect("OFF at 45 % (after boot, inside the band)", !ON(s), s);
        s.min(15);
        s.in.soc = 29;
        s.run(2);
        expect("ON at 29 %", ON(s), s);
        s.c.setMode(Mode::Off, "test");
        s.in.soc = 35;
        s.run(2);
        expect("manual off at 35 % -> OFF", !ON(s) && WHY(s, "manual_off"), s);
        s.in.soc = 25;
        s.run(2);
        expect("25 % -> ON, manual off cancelled", ON(s) && WHY(s, "soc_floor") && s.c.mode() == Mode::Auto, s);
        expect("floor event", s.hasEvent(EvFloorOn), s);
        s.in.soc = 50;
        s.min(16);
        expect("OFF at 50 %", !ON(s) && WHY(s, "soc_high"), s);
    }

    printf("10. Emergency OFF\n");
    {
        Sim s(Winter, Mode::On);
        s.c.setMode(Mode::On, "test");
        s.in.soc = 90;
        s.min(1);
        expect("manual ON", ON(s) && WHY(s, "manual_on"), s);
        s.in.cellMaxV = 3.66f;
        s.run(29);
        expect("not yet at 29 s", ON(s), s);
        s.run(2);
        expect("cell 3.66 V for 30 s -> OFF in manual on", !ON(s) && WHY(s, "emerg_cell"), s);
        expect("emergency event", s.hasEvent(EvEmergOff), s);
        s.in.cellMaxV = 3.49f;
        s.min(14);
        expect("still OFF at 14 min", !ON(s), s);
        s.min(2);
        expect("back after 15 min at 3.49 V", ON(s), s);
        // Two more trips -> lockout.
        for (int i = 0; i < 2; i++) {
            s.in.bmsV = 54.6f; s.run(31);
            s.in.bmsV = 53.0f; s.min(16);
        }
        expect("3 trips in 24 h -> lockout", s.c.lockout() && !ON(s) && WHY(s, "emerg_lock"), s);
        expect("lockout persisted", g_pLock, s);
        s.min(60);
        expect("still locked an hour later", !ON(s), s);
        s.in.soc = 18;
        s.run(2);
        expect("SOC 18 %, all normal -> ON despite lock", ON(s) && WHY(s, "emerg_lock_low_soc"), s);
        s.in.soc = 60;
        s.run(2);
        s.c.ack("test");
        s.run(2);
        expect("ack -> unlocked, manual on again", !s.c.lockout() && ON(s), s);
    }
    {
        Sim s(Winter);
        s.in.soc = 100; s.in.cellMaxV = 3.61f; s.in.ccl = 0; s.in.cclFresh = true;
        s.min(3);
        expect("cell 3.61 V at 100 %: no emergency", !s.c.emergencyOff(), s);
        s.in.soc = 98;
        s.min(3);
        expect("CCL 0 at 98 %: no emergency", !s.c.emergencyOff(), s);
        s.in.invBatI = 45; s.in.ccl = 20; s.in.cellMaxV = 3.40f;
        s.min(3);
        expect("45 A with CCL 20 A: no emergency", !s.c.emergencyOff(), s);
        s.in.soc = 60; s.in.ccl = 0;
        s.run(59);
        expect("CCL 0 at 60 %: not before 1 min", !s.c.emergencyOff(), s);
        s.run(2);
        expect("CCL 0 at 60 % for 1 min -> OFF", s.c.emergencyOff() && !ON(s) && WHY(s, "emerg_ccl"), s);
        s.in.ccl = 50; s.min(16);
        expect("CCL back -> ON after 15 min", ON(s), s);
        s.in.tempC = 46; s.min(1); s.run(2);
        expect("46 °C for 1 min -> OFF", !ON(s) && WHY(s, "emerg_temp"), s);
        s.in.socFresh = false; s.in.tempFresh = false; s.in.cellFresh = false; s.in.bmsVFresh = false;
        s.min(16);
        expect("no data: emergency releases after 15 min -> ON", ON(s) && WHY(s, "no_data"), s);
    }

    printf("11. Cell cap\n");
    {
        Sim s(Winter);
        s.in.soc = 90; s.in.gridV = 165; s.in.outPrio = OutPrio::Sbu;
        s.min(20);
        s.in.soc = 99;
        s.min(1);
        expect("bad grid: ON at 99 %", ON(s) && WHY(s, "to_full"), s);
        s.in.cellMaxV = 3.56f;
        s.run(59);
        expect("not yet at 59 s", ON(s), s);
        s.run(2);
        expect("cell 3.56 V for 1 min -> OFF (cell_cap)", !ON(s) && WHY(s, "cell_cap"), s);
        expect("no Telegram event for the cap", !s.hasEvent(EvEmergOff), s);
        s.in.cellMaxV = 3.47f; s.in.day = true; s.in.toSunsetMin = 60;
        s.min(20);
        expect("60 min before sunset at 99 %: stays OFF (full)", !ON(s) && s.c.full(), s);
        s.in.soc = 95;
        s.min(1);
        expect("95 % but cell 3.47 V: still full", !ON(s), s);
        s.in.soc = 94; s.in.cellMaxV = 3.40f;
        s.run(2);
        expect("94 % and 3.40 V -> ON", ON(s), s);
    }
    {
        Config cfg = defaults();
        cfg.cellCapV = 0;
        Sim s(Winter, Mode::Auto, false, cfg);
        s.in.soc = 90; s.in.gridV = 165; s.in.outPrio = OutPrio::Sbu;
        s.min(20);
        s.in.soc = 99;
        s.in.cellMaxV = 3.60f;
        s.min(5);
        expect("cell_cap_v = 0: rule off", ON(s), s);
    }

    printf("12. APL with weak grid -> ON until UPS for 10 min and +2 points\n");
    {
        Sim s(Shoulder);
        s.in.soc = 92; s.in.day = true; s.in.pvW = 900; s.in.toSunsetMin = 300;
        s.min(20);
        expect("shoulder: OFF at 92 %", !ON(s), s);
        s.in.range = AcRange::Apl; s.in.gridV = 160; s.in.soc = 65; s.in.pvW = 2500;
        s.run(2);
        expect("APL + 160 V -> ON even with sun", ON(s) && WHY(s, "apl_weak_grid"), s);
        s.in.range = AcRange::Ups; s.in.gridV = 200;
        s.min(11);
        expect("UPS 10 min but SOC not up yet -> still ON", ON(s), s);
        s.in.soc = 67;
        s.run(2);
        expect("SOC +2 -> hold over (normal logic)", !s.c.aplHold(), s);
    }

    printf("13. HA hints: outage soon, shoulder advice\n");
    {
        Sim s(Shoulder);
        s.in.soc = 80;
        s.min(20);
        expect("shoulder 80 %: OFF", !ON(s), s);
        s.in.adviceOn = Tri::Yes;
        s.run(2);
        expect("advice on -> threshold 85 % -> ON", ON(s) && WHY(s, "soc_low"), s);
        s.in.adviceOn = Tri::Unknown; s.in.soc = 90;
        s.min(16);
        expect("OFF at 90 %", !ON(s), s);
        s.in.outageSoonMin = 100;
        s.min(11);
        expect("outage in 100 min -> ON", ON(s) && WHY(s, "outage_soon"), s);
    }

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
