#include "compat.h"

#if defined(ESP32)
#include <esp_task_wdt.h>

ResetKind compatResetKind() {
    switch (esp_reset_reason()) {
        case ESP_RST_POWERON:  return ResetKind::PowerOn;
        case ESP_RST_SW:       return ResetKind::Software;
        case ESP_RST_PANIC:    return ResetKind::Crash;
        case ESP_RST_INT_WDT:
        case ESP_RST_TASK_WDT:
        case ESP_RST_WDT:      return ResetKind::Watchdog;
        case ESP_RST_BROWNOUT: return ResetKind::Brownout;
        default:               return ResetKind::Other;
    }
}

void compatFeedWatchdog() { esp_task_wdt_reset(); }

#elif defined(ESP8266)
#include <LittleFS.h>
#include <user_interface.h>

ResetKind compatResetKind() {
    switch (ESP.getResetInfoPtr()->reason) {
        case REASON_DEFAULT_RST:      return ResetKind::PowerOn;
        case REASON_SOFT_RESTART:     return ResetKind::Software;
        case REASON_EXCEPTION_RST:    return ResetKind::Crash;
        case REASON_WDT_RST:
        case REASON_SOFT_WDT_RST:     return ResetKind::Watchdog;
        case REASON_EXT_SYS_RST:      return ResetKind::Other;
        default:                      return ResetKind::Other;
    }
}

void compatFeedWatchdog() { ESP.wdtFeed(); yield(); }

static bool s_fsReady = false;

bool Preferences::begin(const char* ns, bool readOnly) {
    if (!s_fsReady) {
        s_fsReady = LittleFS.begin();
        if (!s_fsReady) {   // first boot on a fresh partition
            LittleFS.format();
            s_fsReady = LittleFS.begin();
        }
    }
    _dir = String("/prefs/") + ns;
    _ro = readOnly;
    if (!readOnly && s_fsReady) LittleFS.mkdir(_dir);
    return s_fsReady;
}

size_t Preferences::readRaw(const char* k, void* buf, size_t len) {
    if (!s_fsReady) return 0;
    File f = LittleFS.open(path(k), "r");
    if (!f) return 0;
    size_t n = f.read((uint8_t*)buf, len);
    f.close();
    return n;
}

size_t Preferences::writeRaw(const char* k, const void* buf, size_t len) {
    if (!s_fsReady || _ro) return 0;
    File f = LittleFS.open(path(k), "w");
    if (!f) return 0;
    size_t n = f.write((const uint8_t*)buf, len);
    f.close();
    return n;
}

size_t Preferences::getBytesLength(const char* k) {
    if (!s_fsReady) return 0;
    File f = LittleFS.open(path(k), "r");
    if (!f) return 0;
    size_t n = f.size();
    f.close();
    return n;
}

String Preferences::getString(const char* k, const String& def) {
    if (!s_fsReady) return def;
    File f = LittleFS.open(path(k), "r");
    if (!f) return def;
    String s = f.readString();
    f.close();
    return s;
}
#endif

const char* compatResetName(ResetKind k) {
    switch (k) {
        case ResetKind::PowerOn:  return "power-on";
        case ResetKind::Software: return "software restart";
        case ResetKind::Crash:    return "crash (panic)";
        case ResetKind::Watchdog: return "watchdog";
        case ResetKind::Brownout: return "brownout (power dip)";
        default:                  return "other";
    }
}
