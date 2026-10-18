#ifndef COMPAT_H
#define COMPAT_H

// -----------------------------------------------------------------------------
// Thin ESP32 / ESP8266 compatibility layer for the relay firmware, so the same
// relay / settings / alerts / web / HA code builds for both boards:
//   * env:esp32dev — the full bridge (Tuya + relays)
//   * env:nodemcu  — relays only, on the NodeMCU in the shed
// -----------------------------------------------------------------------------
#include <Arduino.h>

#if defined(ESP32)
  #include <WiFi.h>
  #include <WebServer.h>
  #include <Preferences.h>
  #include <freertos/FreeRTOS.h>
  #include <freertos/semphr.h>
  #include <esp_system.h>
  using CompatWebServer = WebServer;

  struct CompatMutex {
      SemaphoreHandle_t h = nullptr;
      void init() { if (!h) h = xSemaphoreCreateMutex(); }
      void lock()   { if (h) xSemaphoreTake(h, portMAX_DELAY); }
      void unlock() { if (h) xSemaphoreGive(h); }
  };
#elif defined(ESP8266)
  #include <ESP8266WiFi.h>
  #include <ESP8266WebServer.h>
  using CompatWebServer = ESP8266WebServer;

  // Single-threaded: nothing to lock.
  struct CompatMutex {
      void init() {}
      void lock() {}
      void unlock() {}
  };

  // Minimal stand-in for the ESP32 Preferences API on LittleFS: one file per
  // key under /prefs/<namespace>/. Writes are rare (settings, relay latches).
  class Preferences {
  public:
      bool begin(const char* ns, bool readOnly = false);
      void end() {}
      bool   getBool(const char* k, bool def = false)        { uint8_t v; return readRaw(k, &v, 1) == 1 ? v != 0 : def; }
      size_t putBool(const char* k, bool v)                   { uint8_t b = v; return writeRaw(k, &b, 1); }
      uint8_t getUChar(const char* k, uint8_t def = 0)       { uint8_t v; return readRaw(k, &v, 1) == 1 ? v : def; }
      size_t putUChar(const char* k, uint8_t v)               { return writeRaw(k, &v, 1); }
      int32_t getInt(const char* k, int32_t def = 0)         { int32_t v; return readRaw(k, &v, 4) == 4 ? v : def; }
      size_t putInt(const char* k, int32_t v)                 { return writeRaw(k, &v, 4); }
      uint32_t getUInt(const char* k, uint32_t def = 0)      { uint32_t v; return readRaw(k, &v, 4) == 4 ? v : def; }
      size_t putUInt(const char* k, uint32_t v)               { return writeRaw(k, &v, 4); }
      String getString(const char* k, const String& def = String());
      size_t putString(const char* k, const String& v)        { return writeRaw(k, v.c_str(), v.length()); }
      size_t getBytesLength(const char* k);
      size_t getBytes(const char* k, void* buf, size_t len)   { return readRaw(k, buf, len); }
      size_t putBytes(const char* k, const void* buf, size_t len) { return writeRaw(k, buf, len); }
  private:
      String _dir;
      bool   _ro = false;
      String path(const char* k) const { return _dir + "/" + k; }
      size_t readRaw(const char* k, void* buf, size_t len);
      size_t writeRaw(const char* k, const void* buf, size_t len);
  };
#else
  #error "unsupported board"
#endif

// Why the board started, in terms both chips can express.
enum class ResetKind : uint8_t { PowerOn, Software, Crash, Watchdog, Brownout, Other };
ResetKind compatResetKind();
const char* compatResetName(ResetKind k);   // English, for logs / JSON

// Feed whatever watchdog the platform has (long uploads etc.).
void compatFeedWatchdog();

// Crash "black box" (ESP8266): the exception that reset the board, saved in
// RTC memory by the core's crash hook. Empty string if the last reset wasn't
// a crash (or on ESP32, where the core prints its own backtrace).
String compatLastCrash();

// HMAC-SHA256 as lowercase hex (signed commands from the VPS bridge).
String compatHmacSha256Hex(const String& key, const String& msg);

#endif
