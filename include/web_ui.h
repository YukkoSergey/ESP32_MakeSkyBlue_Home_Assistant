#ifndef WEB_UI_H
#define WEB_UI_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include "compat.h"
#include <functional>
#include "relay_controller.h"

// -----------------------------------------------------------------------------
// Web debug UI on port 80 (LAN), HTTP Basic auth (WEB_USERNAME/WEB_PASSWORD):
//
//   GET  /                  browser -> UI page; anything else -> the old
//                           ESP8266 JSON (no auth, read-only, kept for
//                           whatever still polls it)
//   GET  /api/status        relays + system state
//   GET  /api/log?since=N   log text after position N; X-Log-Pos = new end
//   GET  /api/settings      thresholds / capacity / season mode
//   POST /api/settings      same JSON back; validated, saved to NVS
//   POST /api/gen_mode      body: auto | run | off
//   POST /api/reset_state   relays off, generator mode auto
//   POST /api/reboot
//   POST /update            multipart firmware.bin upload (OTA)
// -----------------------------------------------------------------------------
namespace web_ui {
    using SystemFiller = std::function<void(JsonObject)>;
    void begin(CompatWebServer& server, RelayController& relays, SystemFiller fillSystem);
}

#endif
