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

**Flow C — battery reserve → relays (heavy load + generator)**

Replaces the ESP8266 NodeMCU that used to sit in the shed. Two active-LOW
relay modules:

NodeMCU (env:nodemcu, the board in the shed now): red "2 Relay Module
high/low level trigger" (JQC-3F-05VDC), S1/S2 on **High**, active-HIGH:
DC+ → **VU** (USB 5 V — VIN is input-only on this board, 0 V on USB power),
DC− → G, IN1 → D1, IN2 → D2. Use a real 5 V supply: the old one gave ~8 V.

ESP32 variant:

| Relay | ESP32 pin | Old NodeMCU pin | Drives |
|---|---|---|---|
| Heavy load | GPIO25 | D1 (GPIO5) | 24 V PSU → signal line → contactor in the house (boiler, floor heating, AC / heat pump may run from the batteries) |
| Generator | GPIO26 | D2 (GPIO4) | 2-wire autostart, contact held closed = runs |

- Input: `solar_assistant/total/battery_state_of_charge/state` on the LAN
  broker. Reserve = SOC % × array capacity; all thresholds are kWh.
- Three season profiles (summer / shoulder / winter) with their own
  thresholds; picked by date (Kyiv time) or forced manually. No NTP yet →
  winter, the strictest. Defaults come from Solar Assistant history — see
  the comments in `src/settings.cpp`:

  | Season | Heavy ON > | Heavy OFF < | Gen START < | Gen STOP > |
  |---|---|---|---|---|
  | Summer | 6.0 kWh | 4.0 | 1.5 (emergency) | 3.5 |
  | Shoulder | 7.0 | 5.0 | 2.5 | 5.5 |
  | Winter | 8.2 | 6.5 | 3.0 | 6.5 |
- Generator modes: `auto` (by the profile), `run` (manual start, stops by
  itself at the profile's STOP threshold), `off` (inhibited).
- No fresh SOC for 3 min → heavy load OFF (economy). A running generator
  keeps running; a stopped one isn't started blind.
- **Frozen SOC.** SA sometimes keeps publishing a stuck SOC when its link to
  the Pylontech BMS breaks — 7 times in the Nov 2024 – Oct 2026 history, from
  an hour to 58 days. The inverter measures the battery on its own, so SOC
  counts as frozen when, since its last change, (a) at night with no grid
  voltage the house used ≥ 1 kWh (the MakeSkyBlue chargers are dark, so all
  of it must come from the battery; works at 100 % too), or (b) below 98 % the
  inverter-measured battery energy (V × I) reached 1.5 kWh. Both paused while
  the generator runs + 30 min (its charger is invisible to the inverter).
  On the history: all 7 cases caught, no false alarms. Frozen = same as
  stale; remembered across reboots.
- **Voltage emergency.** Inverter battery voltage < 47.0 V for 5 min → heavy
  load OFF and generator START in any season (unless inhibited by hand),
  whatever SOC says. History: only ever at SOC 4 % and 11 %; the inverter
  itself shuts down at 45.0 V.
- After a reboot both relays are decided from scratch (off until fresh
  SOC; inside a hysteresis band = off). Only a manual "forbid" survives.
  Loop watchdog 60 s — a reboot releases both relays.
- HA entities via MQTT discovery (`dacha_relays/state`). Commands from HA
  (generator / season mode) are accepted only over TLS
  (`MQTT_HA_CA_CERT` in `secrets.h`); without TLS they're read-only.
- The generator charges the battery through its own LiFePO4 charger, so
  the inverter knows nothing about it and there's no direct "generator
  running" signal. Session energy and time-to-goal are estimates from SA
  `battery_power` (net energy balance), informational only.

Tuya HTTPS runs in its own FreeRTOS task: each call is a fresh TLS
handshake, and in `loop()` that used to starve MQTT keepalives.

**Flow D — Cherokee chargers (relay 3, D6)**

Two Cherokee car1848tn chargers (~1.2–1.7 kW each) wired straight to the
battery, past the inverter, fed from phases A and B through a two-pole
contactor. The coil's neutral runs through relay 3 on its **NC** contact:
relay released = chargers ON, energised = OFF. A dead, hung or rebooting
board leaves the battery charging. No grid = no coil = chargers off,
whatever the board says. Logic: `src/cherokee.cpp` (pure C++, no Arduino),
fed once a second by `RelayController::tickCherokee()`.

Local only: LAN Solar Assistant MQTT, no internet / HA / VPS needed. Order:

1. Emergency OFF (fresh data only): highest cell ≥ 3.65 V or BMS ≥ 54.5 V
   for 30 s; CCL 0 below 95 % SOC for 1 min; battery ≤ 2 / ≥ 45 °C for 1 min.
   Back after ≥ 15 min once cell ≤ 3.50, V ≤ 53.5, CCL > 0, 5–40 °C. Three
   trips in 24 h → OFF until "Прийняв" (alarm `cherokee_lock`), except
   SOC < 20 % with everything normal.
2. Voltage emergency (< 47 V 5 min) or SOC below the season floor → ON,
   manual off cancelled.
3. Manual off (survives a reboot in summer only).
4. Stale / frozen SOC → ON.
5. More than 6 switches an hour → ON for 1 h + alarm `cherokee_flap`.
6. Cell cap (`cell_cap_v`, default 3.55 V, 0 = off) for 1 min → OFF, the
   battery counts as full until SOC ≤ ON threshold and cell ≤ 3.45 V.
7. Manual on.
8. Season logic (SOC %, editable on the web page, NVS key `chcfg`):

   | | Winter | Shoulder | Summer |
   |---|---|---|---|
   | ON below | 95 | 60 (85 if HA advises) | 30 |
   | OFF at | 97 with sun or a charging inverter, else 100 + float | 90 | 50 |
   | Floor | — | 40 | 30 |
   | Goal before sunset (≤ 90 min) | 100 | 90 | — |

   Winter/shoulder also: SA in APL with grid < 170 V → ON until UPS for
   10 min and SOC +2 points. Shoulder: UPS with grid < 170 V and not enough
   sun → ON. Any season: HA says an outage within 2 h → ON.

"Inverter can charge" = device mode Grid, charger priority allows utility,
output priority Utility first, grid ≥ 160 V (APL) / 180 V (UPS), held 5 min.
After switching off for that reason the inverter must show > 5 A of charge
within 3 min, otherwise the chargers come back and the inverter isn't
trusted for 30 min. Min 15 min on / 10 min off.

While the chargers run on the grid (and 30 min after), the "BMS sees more
charge than the inverter" signature doesn't count as the generator and the
frozen-SOC rules pause.

SA's LAN MQTT (checked 2026-11-19) publishes **no**
`ac_input_voltage_range`, `total/battery_voltage`, total cell maximum or
CCL — only its cloud integration (HA) has them. The board takes the cell
maximum and BMS voltage from `battery_1..4/cell_voltage_-_highest` and
`battery_N/voltage`, the APL/UPS range from the HA hint, and the CCL rule
has no data until SA publishes it.

Optional HA hint, retained JSON on `dacha_relays/hint` (HA broker), ignored
after 3 h (by `ts` if present, else by arrival):

```json
{"chargers_advice": "on", "surplus": false, "outage_soon_min": 95,
 "ac_range": "UPS", "ts": 1795041039}
```

Telegram (`/cherokee`) and HA (`select`/sensor `cherokee_mode`, signed
command `cherokee_mode` = auto / on / off through the bridge). Scenario
tests: `c++ -std=c++17 -Iinclude test/cherokee_sim.cpp src/cherokee.cpp -o
/tmp/cherokee_sim && /tmp/cherokee_sim`.

## VPS bridge (vps/dacha-bridge)

A small Python service in the VPS k3s (namespace `homeassistant`, next to HA
and Mosquitto) — see `vps/dacha-bridge/bridge.py` and `k8s/deployment.yaml`:

- MakeSkyBlue MPPT (Tuya Cloud) -> Solar_Manager `msbN/notify` (port of the
  ESP32 bridge; the NodeMCU can't do Tuya HTTPS).
- Solar Assistant values forwarded by the shed board (`dacha_relays/sa/*`)
  -> Tuya virtual MUST device.
- Telegram for board alerts (`dacha_relays/alert`), with "Прийняв" sent back
  as a signed command; watchdogs for "board silent" and "HA down".
  Commands: /status, /gen, /heavy, /cherokee, /help.
- Signed commands `dacha_relays/cmd` = {c, a, t, s}, s = HMAC-SHA256(CMD_SECRET,
  "c|a|t"), ±5 min, monotonic t.

- Home Assistant control: `POST /cmd` on the cluster-internal Service
  `dacha-bridge:8080` (bearer `HA_CMD_TOKEN`) -> the same signed command.
  HA side: `vps/home-assistant/packages/dacha_generator.yaml` (rest_command +
  `script.dacha_board_cmd`) and a "Дача" dashboard view with buttons; risky
  ones (generator run, heavy load on) ask for confirmation. Every HA command is
  mirrored to Telegram. Deploy: `bash vps/home-assistant/deploy_generator_control.sh`.

Secrets live in the k8s Secret `dacha-bridge-env` only (plus the matching
`dacha_bridge_auth` line in HA `secrets.yaml`).

## Alerts (Telegram)

Sent straight from the board, no HA/VPS in the path. Alarms (no SA data,
frozen SOC, voltage emergency) repeat every 10 min until acknowledged with
the button under the message or on the web page; a "resolved" message
follows when the condition clears. Generator start/stop and reboots (with
the reason) are sent once. Offline → queued. `/status` in the chat replies
with the current state. TLS is verified against Go Daddy Root G2.

Setup: create a bot with @BotFather, put `TELEGRAM_BOT_TOKEN` into
`secrets.h`, flash, send `/start` to the bot — the board logs the chat id —
then add `TELEGRAM_CHAT_ID` and flash again. The board never talks to a chat
other than `TELEGRAM_CHAT_ID`.

## Web debug UI

`http://dacha-esp32.local/` (or the board IP) on the LAN, HTTP Basic auth
`WEB_USERNAME` / `WEB_PASSWORD` from `secrets.h`. Shows relay and system
state, a live log (RAM ring buffer, 16 KB), season settings, generator
buttons, reboot, relay state reset and a firmware upload form.
Plain `GET /` without `Accept: text/html` still returns the old ESP8266
JSON. API list: [include/web_ui.h](include/web_ui.h).

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
  relay_controller.h  Heavy-load / generator relays, fail-safe rules.
  cherokee.h          Cherokee chargers on relay 3 (pure logic, tested on the Mac).
  settings.h          Season profiles + capacity, persisted in NVS.
  ha_relays.h         HA MQTT discovery + commands for the relays.
  web_ui.h            Web debug UI + JSON API.
  app_log.h           Log sink: Serial + ring buffer for the web UI.

src/
  main.cpp            Setup / loop, two MQTT clients, backoff, dispatch.
  mppt_bridge.cpp     Tuya DP → raw MpptState. Seeded values for boot.
  solar_manager_protocol.cpp  Notify TLD frame packing, control decode.
  solar_assistant_bridge.cpp  Solar Assistant DP publisher (batched).
  tuya_cloud_client.cpp Token cache, expiry tracking, /token +
                        /iot-03/devices/*/status + /datapoint/report.

platformio.ini        board = esp32dev, min_spiffs partitions (OTA),
                      env:ota for uploads over WiFi.
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

# over WiFi, no cable (password = OTA_PASSWORD in secrets.h)
DACHA_OTA_PASSWORD=... pio run -e ota -t upload
```

Or upload `.pio/build/esp32dev/firmware.bin` from the web UI.

Bench-testing the relays without touching the real SOC topic (the old
ESP8266 listens to it):

```
PLATFORMIO_BUILD_FLAGS='-DRELAY_TOPIC_PREFIX=\"bench/\" -DRELAY_SOC_STALE_MS=30000 -DRELAY_VOLT_EMERGENCY_MS=20000' pio run -t upload
mosquitto_pub -h <lan broker> -t bench/solar_assistant/total/battery_state_of_charge/state -m 29
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
- **Seed window** for boot-time fake data is off
  (`MpptBridge::kFakeSeedWindowMs = 0`). Set e.g. 180000 to get 3 min of
  plausible values after boot during dev.
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
  Until then HA can't send relay commands.
- Optional: MQTT Discovery topics for entities that live outside
  Solar_Manager (Solar Assistant mirror, uptime, etc.).

[Solar_Manager]: https://github.com/maybetaken/Solar_Manager
[ha-makeskyblue-mppt-card]: https://github.com/maybetaken/ha-makeskyblue-mppt-card
[Solar Assistant]: https://solar-assistant.io/
[Tuya Cloud OpenAPI]: https://developer.tuya.com/en/docs/cloud/
[ArduinoJson]: https://arduinojson.org/
[PubSubClient]: https://github.com/knolleary/pubsubclient
