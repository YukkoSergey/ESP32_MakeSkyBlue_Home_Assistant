# ESP32 MakeSkyBlue ↔ Home Assistant Bridge

Firmware for an ESP32-WROOM-32 board that ties three physical
MakeSkyBlue MPPT solar charge controllers into a Home Assistant
dashboard, and mirrors a MUST inverter's state from Home Assistant's
Solar Assistant integration back into a Tuya Cloud virtual device.

The whole thing runs on a single ESP32 — no daughter boards, no serial
RS-485 tap, no Tuya LAN protocol. All Tuya traffic goes through the
official Tuya Cloud Open API over HTTPS.

## What it does

Two independent flows on the same board:

```
                Home Assistant                    Tuya Cloud
                                                (Tuya OpenAPI)
                     ▲                                   ▲
       msbN/notify   │                       HTTPS       │
       msbN/online   │                       DP writes   │
       msbN/diag     │                                   │
                     │                                   │
              Public Mosquitto                           │
              (plaintext MQTT)                           │
                     ▲                                   │
                     │ TCP                               │
                     │                                   │
              ┌──────┴──────┐                            │
              │             │                            │
              │    ESP32    ├────────────────────────────┘
              │             │       HTTPS + HMAC-SHA256
              └──────┬──────┘
                     │
                     │ MQTT (LAN)
                     ▼
              LAN Mosquitto
                     ▲
                     │
              Solar Assistant
              (MUST inverter + Pylontech BMS)
```

**Flow A — MakeSkyBlue MPPT → Home Assistant (Solar_Manager)**

1. Every 10 s, poll each of the three MakeSkyBlue devices via Tuya's
   legacy DP endpoint (`/v1.0/iot-03/devices/{id}/status`).
2. Map the DP values into raw Modbus register slots that match
   `custom_components/solar_manager/device_protocol/makeskybluemppt.json`
   from the [Solar_Manager] HA integration.
3. Publish binary TLD Modbus frames every 5 s to `msb1/notify`,
   `msb2/notify`, `msb3/notify` on the VPS mosquitto broker. Publish
   `msbN/diagnostics` every 30 s and `msbN/online` on (re)connect.
4. Solar_Manager parses the frames and updates HA entities
   (`sensor.makeskyblue_mppt_msb1_battery_voltage`, etc). The
   [ha-makeskyblue-mppt-card] Lovelace card renders the dashboard.

For the first 3 minutes after boot the firmware serves seeded plausible
daytime values instead of hitting Tuya — the card lights up immediately
on flash, even at night when the real DPs would all be zero.

**Flow B — Solar Assistant → Tuya virtual MUST device**

1. Subscribe to `solar_assistant/total/#` and `solar_assistant/inverter_1/#`
   on the LAN broker.
2. Cache latest values per topic, mark dirty on change.
3. Every 10 s, batch-report changed DPs to the Tuya virtual MUST
   device via `POST /v1.0/cloud/thing/devices/{id}/datapoint/report`.
   Unchanged values are skipped.

The MUST device on Tuya is a virtual device — SmartLife app shows the
data we push, but doesn't refresh on its own between our writes.

## Repository layout

```
include/
  secrets.h.example   Template for real credentials — copy to secrets.h.
  secrets.h           Gitignored. Real WiFi / MQTT / Tuya credentials.
  config.h            (removed — settings live in secrets.h)
  mppt_bridge.h       Public API + MpptState register model.
  solar_manager_protocol.h  SolarManagerDevice — one instance per MSB.
  solar_assistant_bridge.h  Solar Assistant → Tuya virtual DP mapper.
  solar_assistant_mapping.h Topic ↔ Tuya DP table.
  tuya_cloud_client.h Tuya Cloud OpenAPI client (HMAC signing, token).
  sm_config.h         SmSegment struct shared by SolarManagerDevice.

src/
  main.cpp            Setup / loop, two MQTT clients, backoff, dispatch.
  mppt_bridge.cpp     Tuya DP → raw MpptState. Seeded values for boot.
  solar_manager_protocol.cpp  Notify TLD frame packing, control decode.
  solar_assistant_bridge.cpp  Solar Assistant DP publisher (batched).
  tuya_cloud_client.cpp Token cache, expiry tracking, /token +
                        /iot-03/devices/*/status + /datapoint/report.

platformio.ini        board = esp32dev, loop stack bumped to 16 KB.
```

Two MQTT clients on the same ESP32:

- **LAN client** (`WiFiClient` + `PubSubClient`) — subscribes to Solar
  Assistant topics on `MQTT_LAN_HOST:MQTT_LAN_PORT`, drives Flow B.
- **HA client** (same class, separate instance) — publishes MSB notify
  frames to `MQTT_HA_HOST:MQTT_HA_PORT`, drives Flow A. Also receives
  `/config`, `/control/*`, `/host/heartbeat` from HA.

Both use non-blocking reconnect with exponential backoff (1 s → 60 s).
WiFi and NTP time sync are non-blocking too — Tuya HTTPS signing waits
for real epoch before firing.

## External projects this depends on

| Project | Role |
|---|---|
| [Solar_Manager] | HA custom_component. Parses `msbN/notify` binary Modbus frames, defines the JSON protocol (`makeskybluemppt.json`) we emulate. |
| [ha-makeskyblue-mppt-card] | HACS Lovelace card. Renders the gauge + controls for each `msbN`. |
| [Solar Assistant] | Third-party MUST inverter / Pylontech monitor that publishes `solar_assistant/**` topics on a LAN broker. Not part of this repo. |
| [Tuya Cloud OpenAPI] | HTTPS API for polling MakeSkyBlue DPs and writing to the virtual MUST device. |
| [ArduinoJson] v6 | JSON parse / serialize. Big documents go on the heap. |
| [PubSubClient] | MQTT client. Two instances, one per broker. |

The Python reference emulator that this firmware is derived from lives
outside the repo at `~/homeassistant/tools/mock_makeskyblue.py`. It
implements exactly the same protocol on the wire and is useful for
debugging without flashing.

## The Solar_Manager MQTT protocol

Discovered by reading the integration source:

- `custom_components/solar_manager/plugins/base_device.py`
- `custom_components/solar_manager/plugins/MakeSkyBlueMppt.py`
- `custom_components/solar_manager/protocol_helper/modbus_protocol_helper.py`
- `custom_components/solar_manager/device_protocol/makeskybluemppt.json`

Topic conventions (per device serial `<sn>`):

Published by us:
- `<sn>/online` — firmware version string
- `<sn>/diagnostics` — `{"ssid","rssi","led"}`
- `<sn>/notify` — binary TLD `[slave:1][fn:1][start:2 BE][len:2 BE][data]`

Subscribed by us:
- `<sn>/config` — HA sends `{"segments":[{slave_id,start_address,length,read_command}]}`
- `<sn>/control/cmd` — Modbus write (fn 5 / 6 / 16). Decoded and logged
  only. **WRITE DISABLED IN MVP** — the firmware never writes back to
  the physical MPPT.
- `<sn>/control/led` — `"on"|"off"` toggles diagnostics.led.
- `<sn>/control/restart` — logged, disabled.
- `<sn>/control/reconfig` — resets segments to defaults, republishes online.
- `<sn>/host/heartbeat` — HA pings every 5 s, we timestamp it.

Register key encoding: `key = (fn << 20) | address`. So `0x400005` is
FC4 input register 5 (`battery_voltage`); `0x300001` is FC3 holding
register 1 (`equalization_voltage`). All values are 16-bit big-endian,
some are signed (INT16).

Full scaling table in `include/mppt_bridge.h`.

## Tuya API notes

- **Endpoint discovery is model-dependent.** The MakeSkyBlue MPPTs use
  the legacy DP set — `/v1.0/iot-03/devices/{id}/status` works and
  returns `result` as `[{code, value}, ...]`. The Thing Model path
  `/v1.0/cloud/thing/devices/{id}/properties` returns code 1108 "uri
  path invalid" on these devices.
- **DP write to virtual devices** uses
  `/v1.0/cloud/thing/devices/{id}/datapoint/report` — this endpoint
  works on virtual devices even though they don't have a Thing Model.
- **Signing** is HMAC-SHA256 over
  `client_id + [access_token] + t + nonce + method\nSHA256(body)\n\npath`.
  See `TuyaCloudClient::hmacHex` for the exact composition.
- **Token expiry** — Tuya token TTL is ~7200 s. We store the absolute
  expiry (`_tokenExpiresAtMs`), refresh proactively with a 60 s safety
  margin, and invalidate the cache on API error codes 1010 / 1011.

## Build & flash

```
cp include/secrets.h.example include/secrets.h
# edit secrets.h with real WiFi / MQTT / Tuya values
pio run                     # compile
pio run -t upload           # flash over USB
pio device monitor -b 115200
```

### secrets.h template

Two MQTT brokers, three MakeSkyBlue MPPT IDs, one virtual MUST ID,
three MSB serials (matching Solar_Manager config_entries in HA).

Never commit real values — `secrets.h` is in `.gitignore`.

## Configuration decisions worth knowing

- **Serials `msb1` / `msb2` / `msb3`** are hardcoded in `secrets.h`.
  They must match the Solar_Manager device serials configured in HA
  (Settings → Devices → Solar Manager). Rename either side and both
  will fall out of sync.
- **Seed window** for boot-time fake data is 3 min
  (`MpptBridge::kFakeSeedWindowMs`). Bump it if the card should stay
  "alive" longer during dev.
- **`mapChargeMode`** in `mppt_bridge.cpp` currently returns 0 for any
  Tuya `charge_mode` value and logs the raw string. The Tuya mode
  vocabulary is `mode_1` … `mode_5`; Solar_Manager expects 0..6
  (shutdown / pre_charging / constant_current / constant_voltage /
  mppt_tracking / bus_constant_voltage / float_charging). The
  translation table isn't in yet — waiting to observe real behaviour
  under load before hard-coding a mapping.

## Known TODOs

- `mapChargeMode` mapping table.
- Per-device MQTT client on the HA broker so LWT `<sn>/online = offline`
  fires cleanly on ungraceful disconnect (right now one client covers
  three serials).
- TLS for the VPS broker (currently plaintext over the public internet).
- Optional: MQTT Discovery topics for entities that live outside
  Solar_Manager (Solar Assistant mirror, uptime, etc.).

[Solar_Manager]: https://github.com/maybetaken/Solar_Manager
[ha-makeskyblue-mppt-card]: https://github.com/maybetaken/ha-makeskyblue-mppt-card
[Solar Assistant]: https://solar-assistant.io/
[Tuya Cloud OpenAPI]: https://developer.tuya.com/en/docs/cloud/
[ArduinoJson]: https://arduinojson.org/
[PubSubClient]: https://github.com/knolleary/pubsubclient
