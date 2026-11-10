"""Add (or replace) the "Дача" view on the main storage dashboard.

Runs inside the HA pod: python3 - < add_dacha_view.py
"""
import json

P = "/config/.storage/lovelace.lovelace"


def cmd_button(name, icon, c, a, confirm=None):
    tap = {"action": "perform-action", "perform_action": "script.dacha_board_cmd", "data": {"c": c, "a": a}}
    if confirm:
        tap["confirmation"] = {"text": confirm}
    return {"type": "button", "name": name, "icon": icon, "show_state": False, "tap_action": tap}


def tile(entity, name=None):
    t = {"type": "tile", "entity": entity}
    if name:
        t["name"] = name
    return t


view = {
    "title": "Дача",
    "path": "dacha",
    "icon": "mdi:home-lightning-bolt",
    "type": "sections",
    "max_columns": 2,
    "sections": [
        {"type": "grid", "cards": [
            {"type": "heading", "heading": "Генератор", "icon": "mdi:engine"},
            tile("binary_sensor.dacha_relays_generator_relay", "Реле генератора"),
            tile("binary_sensor.dacha_relays_generator_running_feedback", "Працює (контакт)"),
            tile("sensor.dacha_relays_generator_mode", "Режим"),
            tile("binary_sensor.dacha_relays_generator_did_not_start", "Не завівся"),
            tile("sensor.dacha_relays_generator_session_energy", "Заряджено за сесію"),
            tile("sensor.dacha_relays_generator_time_to_goal", "До мети"),
            cmd_button("Запустити до мети", "mdi:play", "gen_mode", "run",
                       "Запустити генератор? Він працюватиме, доки запас не дійде до мети поточного сезону."),
            cmd_button("Заборонити", "mdi:stop", "gen_mode", "off"),
            cmd_button("Авто", "mdi:autorenew", "gen_mode", "auto"),
        ]},
        {"type": "grid", "cards": [
            {"type": "heading", "heading": "Батарея і важке навантаження", "icon": "mdi:battery"},
            tile("sensor.dacha_relays_battery_soc", "SOC"),
            tile("sensor.dacha_relays_battery_reserve", "Запас"),
            tile("binary_sensor.dacha_relays_heavy_load_allowed", "Важке навантаження"),
            tile("sensor.dacha_relays_heavy_load_mode", "Режим"),
            tile("sensor.dacha_relays_active_season", "Сезон"),
            tile("binary_sensor.dacha_relays_solar_assistant_data", "Дані Solar Assistant"),
            cmd_button("Авто", "mdi:autorenew", "heavy_mode", "auto"),
            cmd_button("Заборонити", "mdi:power-plug-off", "heavy_mode", "off"),
            cmd_button("Дозволити", "mdi:flash", "heavy_mode", "on",
                       "Дозволити важке навантаження незалежно від порогів? "
                       "Аварія по напрузі і робота генератора все одно його вимкнуть."),
        ]},
    ],
}

d = json.load(open(P))
views = d["data"]["config"].setdefault("views", [])
views[:] = [v for v in views if v.get("path") != "dacha"] + [view]
json.dump(d, open(P, "w"), ensure_ascii=False, indent=2)
print("view 'Дача' written,", len(views), "views total")
