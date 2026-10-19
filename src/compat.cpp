#include "compat.h"

volatile uint8_t  g_crumb = 0;
volatile uint16_t g_wifiDrops = 0;

#if defined(ESP32)
#include <esp_task_wdt.h>

String compatLastCrash() { return String(); }
#include <mbedtls/md.h>

String compatHmacSha256Hex(const String& key, const String& msg) {
    uint8_t mac[32];
    mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
                    (const uint8_t*)key.c_str(), key.length(),
                    (const uint8_t*)msg.c_str(), msg.length(), mac);
    char out[65];
    for (int i = 0; i < 32; i++) sprintf(out + i * 2, "%02x", mac[i]);
    return String(out);
}

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
#include <bearssl/bearssl_hmac.h>

// --- crash black box -------------------------------------------------------
// The ESP8266 core calls custom_crash_callback() on an exception, right before
// the reset. RTC user memory survives the reset (not a power loss).
// Besides the exception registers we keep the first code addresses found on
// the stack — a poor man's backtrace (decode with xtensa-lx106-elf-addr2line
// against the saved firmware.elf).
static constexpr int kCrashTrace = 16;
struct CrashRecord {
    uint32_t magic, reason, exccause, epc1, epc2, epc3, excvaddr, depc, uptimeS, freeHeap;
    uint32_t crumb, wifiDrops;
    uint32_t trace[kCrashTrace];
};
static constexpr uint32_t kCrashMagic = 0xDAC4A003;

static bool isCodeAddr(uint32_t a) {
    return (a >= 0x40200000 && a < 0x40300000) || (a >= 0x40100000 && a < 0x40108000);
}

extern "C" void custom_crash_callback(struct rst_info* ri, uint32_t stack, uint32_t stack_end) {
    CrashRecord r{kCrashMagic, ri->reason, ri->exccause, ri->epc1, ri->epc2, ri->epc3,
                  ri->excvaddr, ri->depc, (uint32_t)(millis() / 1000), ESP.getFreeHeap(),
                  g_crumb, g_wifiDrops, {}};
    int n = 0;
    for (uint32_t p = stack; p + 4 <= stack_end && n < kCrashTrace; p += 4) {
        uint32_t v = *(uint32_t*)p;
        if (isCodeAddr(v)) r.trace[n++] = v;
    }
    ESP.rtcUserMemoryWrite(0, (uint32_t*)&r, sizeof(r));
}

String compatLastCrash() {
    CrashRecord r;
    if (!ESP.rtcUserMemoryRead(0, (uint32_t*)&r, sizeof(r)) || r.magic != kCrashMagic) return String();
    uint32_t zero = 0;
    ESP.rtcUserMemoryWrite(0, &zero, sizeof(zero));   // report once
    char buf[160];
    snprintf(buf, sizeof(buf), "exccause=%u epc1=0x%08x epc2=0x%08x excvaddr=0x%08x depc=0x%08x "
             "after %us, free heap %u", r.exccause, r.epc1, r.epc2, r.excvaddr, r.depc, r.uptimeS, r.freeHeap);
    String s(buf);
    snprintf(buf, sizeof(buf), "; loop phase %u, wifi drops %u", r.crumb, r.wifiDrops);
    s += buf;
    s += "; trace:";
    for (int i = 0; i < kCrashTrace && r.trace[i]; i++) {
        snprintf(buf, sizeof(buf), " %08x", r.trace[i]);
        s += buf;
    }
    return s;
}

String compatHmacSha256Hex(const String& key, const String& msg) {
    br_hmac_key_context kc;
    br_hmac_key_init(&kc, &br_sha256_vtable, key.c_str(), key.length());
    br_hmac_context hc;
    br_hmac_init(&hc, &kc, 0);
    br_hmac_update(&hc, msg.c_str(), msg.length());
    uint8_t mac[32];
    br_hmac_out(&hc, mac);
    char out[65];
    for (int i = 0; i < 32; i++) sprintf(out + i * 2, "%02x", mac[i]);
    return String(out);
}

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
