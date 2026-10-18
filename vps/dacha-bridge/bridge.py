"""
dacha-bridge — runs next to Home Assistant on the VPS (k3s, namespace
homeassistant). Everything cloud-to-cloud that a board in the shed can't or
shouldn't do:

  1. MakeSkyBlue MPPT (Tuya Cloud) -> Solar_Manager frames on <sn>/notify,
     a 1:1 port of the ESP32 bridge (mppt_bridge.cpp + solar_manager_protocol.cpp).
  2. Solar Assistant values (forwarded by the shed board to dacha_relays/sa/*)
     -> Tuya virtual MUST device (datapoint/report).
  3. Telegram: board alerts (dacha_relays/alert) -> bot, with a "Прийняв"
     button; /status. Repeats of an acknowledged alarm are dropped here, and a
     signed ack goes back to the board.
  4. Watchdogs that only an outside observer can do: the shed board went
     silent; Home Assistant stopped answering.

Config comes from environment variables (k8s Secret). Small state (Telegram
chat, daily-generation baselines) lives in retained MQTT topics, so the pod
needs no volume.
"""

from __future__ import annotations

import hashlib
import hmac
import json
import logging
import os
import threading
import time
import uuid
from datetime import datetime
from zoneinfo import ZoneInfo

import paho.mqtt.client as mqtt
import requests

log = logging.getLogger("dacha-bridge")
logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")

TZ = ZoneInfo("Europe/Kyiv")
E = os.environ.get

MQTT_HOST = E("MQTT_HOST", "mosquitto.homeassistant.svc.cluster.local")
MQTT_PORT = int(E("MQTT_PORT", "1883"))
MQTT_USER = E("MQTT_USER", "")
MQTT_PASS = E("MQTT_PASS", "")

TUYA_BASE = E("TUYA_BASE", "https://openapi.tuyaeu.com")
TUYA_ID = E("TUYA_ID", "")
TUYA_SECRET = E("TUYA_SECRET", "")
TUYA_MPPT_IDS = [x for x in E("TUYA_MPPT_IDS", "").split(",") if x]
TUYA_VIRTUAL_ID = E("TUYA_VIRTUAL_ID", "")
MSB_SERIALS = [x for x in E("MSB_SERIALS", "msb1,msb2,msb3").split(",") if x]

TG_TOKEN = E("TG_TOKEN", "")
CMD_SECRET = E("CMD_SECRET", "")
HA_URL = E("HA_URL", "http://homeassistant.homeassistant.svc.cluster.local:8123")

FIRMWARE = "vps-bridge-1.0"
BOARD_SILENT_S = 5 * 60
HA_DOWN_FAILS = 3          # consecutive failed checks, one per minute
ALARM_REPEAT_S = 30 * 60   # our own watchdog alarms


# =============================================================================
# Tuya Cloud OpenAPI
# =============================================================================
class Tuya:
    def __init__(self):
        self.token = ""
        self.expires_at = 0.0
        self.s = requests.Session()

    @staticmethod
    def _sha256(b: bytes) -> str:
        return hashlib.sha256(b).hexdigest()

    def _sign(self, msg: str) -> str:
        return hmac.new(TUYA_SECRET.encode(), msg.encode(), hashlib.sha256).hexdigest().upper()

    def _headers(self, method: str, path: str, body: bytes, with_token: bool) -> dict:
        t = str(int(time.time() * 1000))
        nonce = uuid.uuid4().hex
        string_to_sign = f"{method}\n{self._sha256(body)}\n\n{path}"
        prefix = TUYA_ID + (self.token if with_token else "")
        h = {"client_id": TUYA_ID, "t": t, "nonce": nonce, "sign_method": "HMAC-SHA256",
             "sign": self._sign(prefix + t + nonce + string_to_sign)}
        if with_token:
            h["access_token"] = self.token
        return h

    def ensure_token(self) -> bool:
        if self.token and time.time() + 60 < self.expires_at:
            return True
        path = "/v1.0/token?grant_type=1"
        try:
            r = self.s.get(TUYA_BASE + path, headers=self._headers("GET", path, b"", False), timeout=10)
            j = r.json()
        except Exception as e:
            log.warning("tuya token: %s", e)
            return False
        if not j.get("success"):
            log.warning("tuya token error: %s", j)
            self.token = ""
            return False
        self.token = j["result"]["access_token"]
        self.expires_at = time.time() + int(j["result"].get("expire_time", 7200))
        log.info("tuya token ok, %ss", j["result"].get("expire_time"))
        return True

    def _call(self, method: str, path: str, body: dict | None = None):
        if not self.ensure_token():
            return None
        raw = json.dumps(body, separators=(",", ":")).encode() if body is not None else b""
        h = self._headers(method, path, raw, True)
        if body is not None:
            h["Content-Type"] = "application/json"
        try:
            r = self.s.request(method, TUYA_BASE + path, headers=h, data=raw or None, timeout=10)
            j = r.json()
        except Exception as e:
            log.warning("tuya %s %s: %s", method, path, e)
            return None
        if not j.get("success"):
            if j.get("code") in (1010, 1011):
                self.token = ""
            log.warning("tuya %s %s error: %s %s", method, path, j.get("code"), j.get("msg"))
            return None
        return j

    def status(self, device_id: str):
        j = self._call("GET", f"/v1.0/iot-03/devices/{device_id}/status")
        return j["result"] if j else None

    def report(self, device_id: str, dp_id: str, value) -> bool:
        body = {"data": {dp_id: {"value": value, "time": int(time.time() * 1000)}}}
        return self._call("POST", f"/v1.0/cloud/thing/devices/{device_id}/datapoint/report", body) is not None


# =============================================================================
# MakeSkyBlue -> Solar_Manager (port of mppt_bridge.cpp / solar_manager_protocol.cpp)
# =============================================================================
# Tuya charge_mode -> Solar_Manager work_status (see mppt_bridge.cpp for notes).
CHARGE_MODE = {"mode_1": 0, "mode_2": 1, "mode_3": 2, "mode_4": 4, "mode_5": 6}
DEFAULT_SEGMENTS = [(1, 4, 1, 12), (1, 4, 26, 1), (1, 3, 1, 10)]   # slave, fn, start, len


def u16(v) -> int:
    try:
        v = int(v)
    except (TypeError, ValueError):
        return 0
    return max(0, min(0xFFFF, v))


def i16(v) -> int:
    try:
        v = int(v)
    except (TypeError, ValueError):
        return 0
    return max(-32768, min(32767, v)) & 0xFFFF   # two's complement on the wire


class Msb:
    def __init__(self, serial: str, device_id: str):
        self.serial = serial
        self.device_id = device_id
        self.segments = list(DEFAULT_SEGMENTS)
        self.fc4: dict[int, int] = {}
        self.fc3: dict[int, int] = {}
        self.valid = False
        self.baseline = None      # electric_total raw at the start of the local day
        self.baseline_date = None

    def update(self, props: list, bridge: "Bridge"):
        p = {x.get("code"): x.get("value") for x in props}
        et = int(p.get("electric_total") or 0)
        today = datetime.now(TZ).date().isoformat()
        if self.baseline is None or self.baseline_date != today:
            self.baseline, self.baseline_date = et, today
            bridge.save_baselines()
        battery_type = p.get("battery_type")
        self.fc4 = {
            0x01: u16(p.get("fault")),
            0x05: u16(p.get("bat_voltage")),
            0x06: i16(p.get("bat_current")),
            0x07: u16(p.get("pv_voltage")),
            0x08: u16(int(p.get("power") or 0) // 10),           # 0.1 W -> 1 W
            0x09: i16(p.get("temp_current")),
            0x0A: u16(et // 10),                                  # 0.1 kWh -> 1 kWh
            0x0B: 0,                                              # not exposed by Tuya
            0x0C: CHARGE_MODE.get(p.get("charge_mode"), 0),
            0x1A: u16(max(0, et - self.baseline)),                # 0.1 kWh, as is
        }
        self.fc3 = {
            0x01: u16(p.get("equalization_volt")),
            0x02: u16(p.get("float_charg_volt")),
            0x03: u16(p.get("output_timr")),
            0x04: u16(int(p.get("constant_cur_set") or 0) * 10),  # A -> 0.1 A
            0x05: u16(p.get("undervol_value")),
            0x06: u16(p.get("uv_reco_value")),
            0x07: 1,
            0x08: 1 if str(battery_type).lower() in ("lithium", "li") else 0,
            0x09: u16(p.get("battery_cell")),
            0x0A: 0,
        }
        if p.get("charge_mode") not in CHARGE_MODE and p.get("charge_mode"):
            log.info("%s: unknown charge_mode %r", self.serial, p.get("charge_mode"))
        self.valid = True

    def frames(self):
        if not self.valid:
            return
        for slave, fn, start, length in self.segments:
            regs = self.fc4 if fn == 4 else self.fc3 if fn == 3 else {}
            b = bytearray([slave & 0xFF, fn & 0xFF, start >> 8, start & 0xFF, length >> 8, length & 0xFF])
            for a in range(start, start + length):
                v = regs.get(a, 0)
                b += bytes([v >> 8, v & 0xFF])
            yield bytes(b)


# =============================================================================
# Solar Assistant (via the shed board) -> Tuya virtual MUST device
# =============================================================================
SA_DPS = {   # dacha_relays/sa/<code>  ->  Tuya DP id (solar_assistant_mapping.h)
    "battery_percentage": "101", "grid_voltage": "102", "pv_power": "103",
    "load_power": "104", "grid_load": "105", "battery_load": "106",
}


def tuya_value(s: str):
    try:
        return float(s) if "." in s else int(s)
    except ValueError:
        return s


# =============================================================================
# Telegram
# =============================================================================
class Telegram:
    def __init__(self, bridge: "Bridge"):
        self.b = bridge
        self.api = f"https://api.telegram.org/bot{TG_TOKEN}/"
        self.s = requests.Session()
        self.offset = 0

    def call(self, method: str, **kw):
        try:
            r = self.s.post(self.api + method, json=kw, timeout=40)
            j = r.json()
            if not j.get("ok"):
                log.warning("telegram %s: %s", method, j.get("description"))
            return j
        except Exception as e:
            log.warning("telegram %s: %s", method, e)
            return None

    def send(self, text: str, ack_key: str | None = None, buttons: list | None = None):
        chat = self.b.tg_chat
        if not chat:
            log.info("telegram: no chat yet (send /start to the bot); dropped: %s", text[:80])
            return
        kw = {"chat_id": chat, "text": text}
        if ack_key:
            kw["reply_markup"] = {"inline_keyboard": [[{"text": "✅ Прийняв", "callback_data": "ack:" + ack_key}]]}
        elif buttons:
            kw["reply_markup"] = {"inline_keyboard": [[{"text": t, "callback_data": d} for t, d in row]
                                                      for row in buttons]}
        self.call("sendMessage", **kw)

    def setup_menu(self):
        self.call("setMyCommands", commands=[
            {"command": "status", "description": "Поточний стан"},
            {"command": "gen", "description": "Генератор: запустити / заборонити / авто"},
            {"command": "heavy", "description": "Важке навантаження: авто / дозволити / заборонити"},
        ])

    def poll_forever(self):
        while True:
            j = self.call("getUpdates", offset=self.offset, timeout=30,
                          allowed_updates=["message", "callback_query"])
            if not j or not j.get("ok"):
                time.sleep(10)
                continue
            for u in j["result"]:
                self.offset = u["update_id"] + 1
                try:
                    self.handle(u)
                except Exception:
                    log.exception("telegram update")

    def handle(self, u: dict):
        if "callback_query" in u:
            cq = u["callback_query"]
            chat = str(cq["message"]["chat"]["id"])
            data = cq.get("data", "")
            if chat != self.b.tg_chat:
                self.call("answerCallbackQuery", callback_query_id=cq["id"])
                return
            if data.startswith("ack:"):
                key = data[4:]
                self.b.acked.add(key)
                self.b.send_board_cmd("ack", key)
                self.call("answerCallbackQuery", callback_query_id=cq["id"], text="Прийнято, нагадувань більше не буде")
                log.info("alarm %s acknowledged in Telegram", key)
            elif data.startswith("ask:"):        # risky command: confirm first
                _, cmd, arg = data.split(":", 2)
                self.call("answerCallbackQuery", callback_query_id=cq["id"])
                self.send(CONFIRM_TEXT.get((cmd, arg), "Підтвердити?"),
                          buttons=[[("✅ Так", f"do:{cmd}:{arg}"), ("✖️ Ні", "noop")]])
            elif data.startswith("do:"):
                _, cmd, arg = data.split(":", 2)
                self.call("answerCallbackQuery", callback_query_id=cq["id"], text="Надсилаю на плату…")
                threading.Thread(target=self.b.run_board_command, args=(cmd, arg), daemon=True).start()
            else:
                self.call("answerCallbackQuery", callback_query_id=cq["id"])
            return
        msg = u.get("message") or {}
        chat = str(msg.get("chat", {}).get("id", ""))
        text = (msg.get("text") or "").strip()
        if not chat:
            return
        if not self.b.tg_chat and text.startswith("/start"):
            self.b.set_tg_chat(chat)
            self.send("✅ Дача: цей чат тепер отримує тривоги. /status — поточний стан.")
            return
        if chat != self.b.tg_chat:
            log.info("telegram: ignoring chat %s", chat)
            return
        if text.startswith("/gen"):
            self.send(self.b.gen_text(), buttons=[
                [("▶️ Запустити до мети", "ask:gen_mode:run")],
                [("⏹ Заборонити", "do:gen_mode:off"), ("🔄 Авто", "do:gen_mode:auto")]])
        elif text.startswith("/heavy"):
            self.send(self.b.heavy_text(), buttons=[
                [("🔄 Авто", "do:heavy_mode:auto"), ("⛔ Заборонити", "do:heavy_mode:off")],
                [("⚡ Дозволити (обхід порогів)", "ask:heavy_mode:on")]])
        elif text.startswith("/status") or text.startswith("/start"):
            self.send(self.b.status_text())
        else:
            self.send("Команди: /status, /gen, /heavy")


CONFIRM_TEXT = {
    ("gen_mode", "run"): "Запустити генератор? Він працюватиме, доки запас не дійде до мети поточного сезону.",
    ("heavy_mode", "on"): "Дозволити важке навантаження незалежно від порогів? "
                          "Аварія за напругою і робота генератора все одно його вимкнуть.",
}
MODE_UA = {"auto": "авто", "run": "запуск до мети", "off": "заборонено", "on": "дозволено вручну"}


# =============================================================================
# The bridge
# =============================================================================
class Bridge:
    def __init__(self):
        self.tuya = Tuya()
        self.msbs = [Msb(s, d) for s, d in zip(MSB_SERIALS, TUYA_MPPT_IDS)]
        self.sa_values: dict[str, str] = {}
        self.sa_sent: dict[str, str] = {}
        self.tg_chat = ""
        self.acked: set[str] = set()
        self.board_state: dict = {}
        self.board_last = 0.0
        self.watch = {}   # our own alarms: key -> {"active", "since", "last"}
        self.ha_lwt_offline = False
        self.mq = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id="dacha-bridge")
        self.mq.username_pw_set(MQTT_USER, MQTT_PASS)
        self.mq.will_set(f"{MSB_SERIALS[0]}/online", "offline", qos=1)
        self.mq.on_connect = self.on_connect
        self.mq.on_message = self.on_message
        self.tg = Telegram(self)

    # --- MQTT ---------------------------------------------------------------
    def on_connect(self, c, userdata, flags, rc, props=None):
        log.info("mqtt connected rc=%s", rc)
        subs = ["dacha_relays/state", "dacha_relays/alert", "dacha_relays/sa/#",
                "dacha_bridge/tg_chat", "dacha_bridge/msb_baseline", "homeassistant/status"]
        for m in self.msbs:
            subs += [f"{m.serial}/config", f"{m.serial}/control/#", f"{m.serial}/host/#"]
        for t in subs:
            c.subscribe(t)
        for m in self.msbs:
            c.publish(f"{m.serial}/online", FIRMWARE)

    def on_message(self, c, userdata, msg):
        t = msg.topic
        try:
            if t == "dacha_relays/state":
                self.board_state = json.loads(msg.payload)
                self.board_last = time.time()
            elif t == "dacha_relays/alert":
                self.on_alert(json.loads(msg.payload))
            elif t.startswith("dacha_relays/sa/"):
                self.sa_values[t.rsplit("/", 1)[1]] = msg.payload.decode().strip()
            elif t == "dacha_bridge/tg_chat":
                self.tg_chat = msg.payload.decode().strip()
            elif t == "dacha_bridge/msb_baseline":
                d = json.loads(msg.payload or b"{}")
                for m in self.msbs:
                    if m.serial in d and m.baseline is None:
                        m.baseline_date, m.baseline = d[m.serial]
            elif t == "homeassistant/status":
                self.ha_lwt_offline = msg.payload.decode().strip() == "offline"
            else:
                for m in self.msbs:
                    if t == f"{m.serial}/config":
                        segs = json.loads(msg.payload).get("segments") or []
                        m.segments = [(int(s.get("slave_id", 1)), int(s.get("read_command", 4)),
                                       int(s.get("start_address", 0)), int(s.get("length", 0)))
                                      for s in segs[:5]]
                        log.info("%s: /config %d segments", m.serial, len(m.segments))
                    elif t == f"{m.serial}/control/reconfig":
                        m.segments = list(DEFAULT_SEGMENTS)
                        c.publish(f"{m.serial}/online", FIRMWARE)
                    elif t == f"{m.serial}/control/cmd":
                        log.info("%s: control/cmd ignored (writes disabled)", m.serial)
        except Exception:
            log.exception("message on %s", t)

    def set_tg_chat(self, chat: str):
        self.tg_chat = chat
        self.mq.publish("dacha_bridge/tg_chat", chat, retain=True)
        log.info("telegram chat bound: %s", chat)

    def save_baselines(self):
        d = {m.serial: [m.baseline_date, m.baseline] for m in self.msbs if m.baseline is not None}
        self.mq.publish("dacha_bridge/msb_baseline", json.dumps(d), retain=True)

    def send_board_cmd(self, cmd: str, arg: str):
        if not CMD_SECRET:
            return
        ts = int(time.time())
        sig = hmac.new(CMD_SECRET.encode(), f"{cmd}|{arg}|{ts}".encode(), hashlib.sha256).hexdigest()
        self.mq.publish("dacha_relays/cmd", json.dumps({"c": cmd, "a": arg, "t": ts, "s": sig}))

    def run_board_command(self, cmd: str, arg: str):
        """Send a signed command and report what the board did (from its state)."""
        s = self.board_state
        if not s or time.time() - self.board_last > BOARD_SILENT_S:
            self.tg.send("⚠️ Плата зараз не на зв'язку — команду не надіслано.")
            return
        if cmd == "gen_mode" and arg == "run":
            goal = (s.get("thresholds") or {}).get("gen_stop_kwh")
            if goal is not None and (s.get("reserve_kwh") or 0) >= goal:
                self.tg.send(f"ℹ️ Запас {s.get('reserve_kwh')} кВт·год уже не нижчий за мету {goal} кВт·год — "
                             "плата одразу поверне «авто», генератор не запуститься.")
                return
        before = time.time()
        self.send_board_cmd(cmd, arg)
        log.info("board command %s=%s from Telegram", cmd, arg)
        key = cmd
        for _ in range(20):                      # the board republishes its state right away
            time.sleep(1)
            if self.board_last > before and self.board_state.get(key) == arg:
                break
        s = self.board_state
        if self.board_last <= before or s.get(key) != arg:
            self.tg.send(f"⚠️ Плата не підтвердила команду ({cmd} = {arg}). Перевір /status.")
            return
        self.tg.send(self.gen_text() if cmd == "gen_mode" else self.heavy_text())

    def gen_text(self) -> str:
        s = self.board_state
        if not s:
            return "Від плати ще немає даних."
        th = s.get("thresholds") or {}
        fb = ""
        if s.get("gen_fb_enabled"):
            fb = " Генератор (контакт): " + ("ПРАЦЮЄ" if s.get("gen_fb_running") else "стоїть") + "."
            if s.get("gen_no_start"):
                fb += " ⚠️ Не завівся!"
        return (f"⚙️ Генератор: режим {MODE_UA.get(s.get('gen_mode'), s.get('gen_mode'))}, реле "
                f"{'ЗАМКНУТЕ' if s.get('generator') else 'розімкнуте'}.{fb}\n"
                f"Запас {s.get('reserve_kwh')} кВт·год; авто-старт < {th.get('gen_start_kwh')}, "
                f"мета / стоп > {th.get('gen_stop_kwh')}.")

    def heavy_text(self) -> str:
        s = self.board_state
        if not s:
            return "Від плати ще немає даних."
        th = s.get("thresholds") or {}
        return (f"🔌 Важке навантаження: {'ДОЗВОЛЕНЕ' if s.get('heavy_load') else 'економ'} "
                f"(режим {MODE_UA.get(s.get('heavy_mode'), s.get('heavy_mode'))}).\n"
                f"Запас {s.get('reserve_kwh')} кВт·год; вмикається > {th.get('heavy_on_kwh')}, "
                f"вимикається < {th.get('heavy_off_kwh')}.")

    # --- alerts from the board ---------------------------------------------
    def on_alert(self, a: dict):
        key, level = a.get("key", ""), a.get("level", "info")
        if level == "alarm":
            self.acked.discard(key)
        if level == "repeat" and key in self.acked:
            return   # acknowledged here; the board may not know yet
        if level == "resolved":
            self.acked.discard(key)
        text = a.get("title", "")
        if a.get("text"):
            text += ".\n" + a["text"]
        self.tg.send(text, key if level in ("alarm", "repeat") else None)

    def status_text(self) -> str:
        s = self.board_state
        if not s:
            return "📊 Дача: від плати ще немає даних."
        age = int(time.time() - self.board_last)
        heavy = "дозволене" if s.get("heavy_load") else "економ"
        gen = "працює" if s.get("generator") or s.get("gen_fb_running") else "стоїть"
        return (f"📊 Дача: SOC {s.get('soc')} %, запас {s.get('reserve_kwh')} кВт·год, сезон {s.get('season')}.\n"
                f"Важке навантаження: {heavy} (режим {s.get('heavy_mode', 'auto')}). "
                f"Генератор: {gen} (режим {s.get('gen_mode')}).\n"
                f"Мережа: {'є' if (s.get('grid_v') or 0) >= 100 else 'немає'}. Дані плати {age} с тому.")

    # --- our own watchdogs ---------------------------------------------------
    def watch_alarm(self, key: str, active: bool, title: str, text: str):
        w = self.watch.setdefault(key, {"active": False, "since": 0, "last": 0})
        now = time.time()
        if active and not w["active"]:
            w.update(active=True, since=now, last=now)
            self.acked.discard(key)
            self.tg.send(f"🚨 Дача: {title}.\n{text}", key)
        elif active and now - w["last"] >= ALARM_REPEAT_S and key not in self.acked:
            w["last"] = now
            self.tg.send(f"🔁 Дача, нагадування: {title}.\n{text}", key)
        elif not active and w["active"]:
            w["active"] = False
            self.acked.discard(key)
            self.tg.send(f"✅ Дача: минуло — {title} (тривало {int((now - w['since']) / 60)} хв).")

    # --- loops -----------------------------------------------------------------
    def loop_mppt(self):
        while True:
            for m in self.msbs:
                r = self.tuya.status(m.device_id)
                if r is not None:
                    m.update(r, self)
            time.sleep(10)

    def loop_publish(self):
        n = 0
        while True:
            for m in self.msbs:
                for f in m.frames():
                    self.mq.publish(f"{m.serial}/notify", f)
                if n % 6 == 0:
                    self.mq.publish(f"{m.serial}/diagnostics",
                                    json.dumps({"ssid": "vps", "rssi": 0, "led": "on"}))
            n += 1
            time.sleep(5)

    def loop_sa(self):
        while True:
            time.sleep(10)
            if not TUYA_VIRTUAL_ID:
                continue
            for code, dp in SA_DPS.items():
                v = self.sa_values.get(code)
                if v is None or self.sa_sent.get(code) == v:
                    continue
                if self.tuya.report(TUYA_VIRTUAL_ID, dp, tuya_value(v)):
                    self.sa_sent[code] = v

    def loop_watch(self):
        ha_fails = 0
        time.sleep(120)   # let the first board state arrive
        while True:
            silent = self.board_last and time.time() - self.board_last > BOARD_SILENT_S
            self.watch_alarm("board_silent", bool(silent or not self.board_last),
                             "плата в сараї мовчить",
                             "Немає даних від плати понад 5 хв. Можливі причини: зникло живлення плати, "
                             "WiFi або інтернет на дачі, плата зависла. Реле без плати відпущені "
                             "(економ, генератор від плати не запуститься).")
            try:
                requests.get(HA_URL + "/api/", timeout=10)
                ha_fails = 0
            except Exception:
                ha_fails += 1
            self.watch_alarm("ha_down_vps", ha_fails >= HA_DOWN_FAILS or self.ha_lwt_offline,
                             "Home Assistant не відповідає",
                             "HA на VPS не відповідає вже кілька хвилин. Push з телефона не буде; "
                             "Telegram працює.")
            time.sleep(60)

    def run(self):
        log.info("starting: %d MPPT, virtual=%s, telegram=%s", len(self.msbs),
                 bool(TUYA_VIRTUAL_ID), bool(TG_TOKEN))
        self.mq.connect_async(MQTT_HOST, MQTT_PORT, keepalive=30)
        self.mq.loop_start()
        time.sleep(3)   # retained chat / baselines first
        for target in (self.loop_mppt, self.loop_publish, self.loop_sa, self.loop_watch):
            threading.Thread(target=target, daemon=True).start()
        if TG_TOKEN:
            self.tg.setup_menu()
            self.tg.poll_forever()
        else:
            while True:
                time.sleep(3600)


if __name__ == "__main__":
    Bridge().run()
