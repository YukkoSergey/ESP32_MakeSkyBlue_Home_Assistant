"""Separate "Tuya" dashboard: every Tuya device in HA, grouped by purpose.

Run inside the HA container (storage-mode dashboard), then restart HA:
  kubectl -n homeassistant exec -i deploy/homeassistant -c homeassistant -- python3 - < add_tuya_dashboard.py
"""
import json, shutil, time

S = "/config/.storage/"
TS = time.strftime("%Y%m%dT%H%M%S")
M = "sensor.3_phase_meter_"


def tile(e, name=None, **kw):
    c = {"type": "tile", "entity": e, "grid_options": {"columns": 6}}
    if name: c["name"] = name
    c.update(kw); return c


def heading(t, icon): return {"type": "heading", "heading": t, "icon": icon}


def graph(title, ents, hours=24): return {"type": "history-graph", "title": title, "hours_to_show": hours, "entities": ents}


def plug(prefix, name, switch="socket_1"):
    return [tile(f"switch.{prefix}_{switch}", name, features=[{"type": "toggle"}], features_position="inline"),
            tile(f"sensor.{prefix}_power", "Потужність")]


phase = lambda p: [tile(f"{M}phase_{p}_voltage", f"Фаза {p.upper()} · напруга"),
                   tile(f"{M}phase_{p}_current", "Струм"),
                   tile(f"{M}phase_{p}_power", "Потужність", grid_options={"columns": 12})]

electric = {"type": "sections", "title": "Електрика", "path": "electric", "icon": "mdi:transmission-tower", "max_columns": 3, "sections": [
    {"type": "grid", "cards": [heading("Лічильник 3 фази", "mdi:meter-electric"),
        tile(f"{M}total_power", "Загальна потужність", grid_options={"columns": 12}),
        tile(f"{M}total_energy", "Спожито"), tile(f"{M}total_production", "Віддано"),
        tile("switch.3_phase_meter_switch", "Реле лічильника", grid_options={"columns": 12})]},
    {"type": "grid", "cards": [heading("Фаза A · Cherokee 1", "mdi:alpha-a-circle")] + phase("a")
        + [heading("Фаза B · Cherokee 2", "mdi:alpha-b-circle")] + phase("b")},
    {"type": "grid", "cards": [heading("Фаза C · Інвертор", "mdi:alpha-c-circle")] + phase("c")},
    {"type": "grid", "column_span": 3, "cards": [
        graph("Напруга по фазах", [f"{M}phase_{p}_voltage" for p in "abc"]),
        graph("Струм по фазах", [f"{M}phase_{p}_current" for p in "abc"])]},
]}

climate = lambda e, n: {"type": "thermostat", "entity": e, "name": n, "grid_options": {"columns": 6}}
heat = {"type": "sections", "title": "Опалення", "path": "heat", "icon": "mdi:radiator", "max_columns": 3, "sections": [
    {"type": "grid", "cards": [heading("Тепла підлога", "mdi:heating-coil"),
        climate("climate.thermostat_bathroom", "Ванна"), climate("climate.thermostat_kid_s_room", "Дитяча"),
        climate("climate.termostat_vannaia", "Ванна (стара)")]},
    {"type": "grid", "cards": [heading("Бойлер", "mdi:water-boiler"),
        climate("climate.boiler_temperature_controller", "Бойлер"),
        heading("Обігрівачі", "mdi:radiator")] + plug("masha_room_heater", "Обігрівач Маші") + plug("home_towel_dryer", "Рушникосушка")},
    {"type": "grid", "cards": [heading("Баласт: надлишок сонця → обігрівач Маші", "mdi:solar-power"),
        tile("binary_sensor.dacha_bridge_balast_obigrivach_mashi_aktivnii", "Баласт активний"),
        tile("sensor.dacha_bridge_balast_khvilin_sogodni", "Сьогодні"),
        tile("sensor.dacha_bridge_balast_godin_za_sezon", "За сезон"),
        tile("sensor.dacha_bridge_balast_energiia_za_sezon", "Енергія за сезон"),
        tile("sensor.dacha_bridge_balast_stan", "Стан", grid_options={"columns": 12})]},
    {"type": "grid", "column_span": 3, "cards": [
        graph("Температура підлоги і бойлера", ["climate.thermostat_bathroom", "climate.thermostat_kid_s_room", "climate.boiler_temperature_controller"]),
        graph("Потужність обігрівачів", ["sensor.masha_room_heater_power", "sensor.home_towel_dryer_power"])]},
]}

sw = lambda e, n, icon=None: tile(e, n, features=[{"type": "toggle"}], features_position="inline", **({"icon": icon} if icon else {}))
devices = {"type": "sections", "title": "Розетки і реле", "path": "switches", "icon": "mdi:power-socket-eu", "max_columns": 3, "sections": [
    {"type": "grid", "cards": [heading("Розетки", "mdi:power-socket-eu")] + plug("ceiling_vents", "Вентиляція стелі")
        + [sw("switch.indoor_lamp_usb_socket_1", "Лампа"), sw("switch.indoor_lamp_usb_socket_2", "USB"),
           tile("sensor.indoor_lamp_usb_power", "Лампа+USB · потужність", grid_options={"columns": 12})]},
    {"type": "grid", "cards": [heading("Реле", "mdi:electric-switch"),
        sw("switch.car_charger_switch_switch_1", "Зарядка авто", "mdi:ev-station"),
        sw("switch.water_pump_switch", "Насос", "mdi:water-pump"),
        sw("switch.whater_filter_switch_switch_1", "Фільтр води", "mdi:filter"),
        sw("switch.screen_street_lights_switch_1", "Вуличне світло", "mdi:outdoor-lamp")]},
    {"type": "grid", "cards": [heading("Датчики живлення", "mdi:flash"),
        tile("switch.generator_sensor_switch_1", "Генератор", icon="mdi:engine"),
        tile("switch.24v_power_supply_on_sensor_switch_1", "БЖ 24 В", icon="mdi:power-plug"),
        tile("sensor.water_pump_total_energy", "Насос · енергія", grid_options={"columns": 12})]},
]}

sensors = {"type": "sections", "title": "Датчики і сцени", "path": "sensors", "icon": "mdi:thermometer", "max_columns": 3, "sections": [
    {"type": "grid", "cards": [heading("Клімат", "mdi:thermometer"),
        tile("sensor.temperatura_i_vlazhnost_dom_temperature", "Дім · t°"), tile("sensor.temperatura_i_vlazhnost_dom_humidity", "Дім · вологість"),
        tile("sensor.shed_thermometer_temperature", "Сарай · t°"), tile("sensor.shed_thermometer_humidity", "Сарай · вологість"),
        tile("sensor.temperatura_i_vlazhnost_dom_battery_state", "Батарея · дім"), tile("sensor.shed_thermometer_battery_state", "Батарея · сарай")]},
    {"type": "grid", "cards": [heading("Протікання", "mdi:water-alert"),
        tile("binary_sensor.water_leakage_detector_bathroom_moisture", "Ванна"), tile("binary_sensor.water_leakage_detector_kitchen_moisture", "Кухня"),
        tile("sensor.water_leakage_detector_bathroom_battery", "Батарея · ванна"), tile("sensor.water_leakage_detector_kitchen_battery", "Батарея · кухня")]},
    {"type": "grid", "cards": [heading("Сцени Tuya", "mdi:play-circle")] + [
        tile(e, n, grid_options={"columns": 12}, tap_action={"action": "perform-action", "perform_action": "scene.turn_on", "target": {"entity_id": e}})
        for e, n in [("scene.ac_obigriv_20", "Кондиціонер: обігрів +20"), ("scene.ac_vimknuti", "Кондиціонер: вимкнути"),
                     ("scene.boil_water_for_1h", "Бойлер на 1 год"), ("scene.bolier_manual_cycle", "Бойлер: ручний цикл"),
                     ("scene.charge_car_for_1_hour", "Зарядка авто 1 год"), ("scene.towel_dryer_switch_1_on", "Рушникосушка ON")]]},
    {"type": "grid", "column_span": 3, "cards": [graph("Температура", ["sensor.temperatura_i_vlazhnost_dom_temperature", "sensor.shed_thermometer_temperature"], 48)]},
]}


ent = lambda e, n=None: {"type": "tile", "entity": e, "grid_options": {"columns": 12}, **({"name": n} if n else {})}
run_report = lambda name, icon, commit: {"type": "tile", "entity": "script.electricity_report", "name": name, "icon": icon,
    "grid_options": {"columns": 6}, "tap_action": {"action": "perform-action", "perform_action": "script.electricity_report",
    "data": {"commit": commit}, "confirmation": {"text": f"{name}?"}}}
REPORT_MD = """{% set c1 = states('sensor.dacha_meter_t1') | float(0) | round(0) | int %}{% set c2 = states('sensor.dacha_meter_t2') | float(0) | round(0) | int %}
{% set p1 = states('input_number.el_prev_t1') | int(0) %}{% set p2 = states('input_number.el_prev_t2') | int(0) %}
{% set r1 = states('input_number.el_tariff_t1') | float(0) %}{% set r2 = states('input_number.el_tariff_t2') | float(0) %}
{% set s1 = ((c1 - p1) * r1) | round(0) | int %}{% set s2 = ((c2 - p2) * r2) | round(0) | int %}
| | Т1 — день | Т2 — ніч |
|---|---:|---:|
| Попередні | {{ p1 }} | {{ p2 }} |
| Поточні | {{ c1 }} | {{ c2 }} |
| Різниця, кВт·год | {{ c1 - p1 }} | {{ c2 - p2 }} |
| Тариф, грн | {{ '%g' | format(r1) }} | {{ '%g' | format(r2) }} |
| Всього, грн | {{ s1 }} | {{ s2 }} |

**Разом Т1+Т2 = {{ s1 + s2 }} грн**

Ділянка {{ states('input_text.el_plot') }} · {{ states('input_text.el_owner') }}<br>
{{ states('input_text.el_membership') }}<br>
Електрика сплачена на "{{ states('input_text.el_paid_to') }}"
"""
bill = {"type": "sections", "title": "Рахунок за електрику", "path": "bill", "icon": "mdi:cash-multiple", "max_columns": 3, "sections": [
    {"type": "grid", "column_span": 2, "cards": [heading("Звіт голові кооперативу (зараз)", "mdi:file-document-outline"),
        {"type": "markdown", "content": REPORT_MD, "grid_options": {"columns": 12}},
        run_report("Сформувати і надіслати", "mdi:file-send", True),
        run_report("Надіслати без збереження", "mdi:file-eye", False),
        ent("input_text.el_last_report")]},
    {"type": "grid", "cards": [heading("Тарифи і поля звіту", "mdi:cog"),
        ent("input_number.el_tariff_t1"), ent("input_number.el_tariff_t2"),
        ent("input_number.el_prev_t1"), ent("input_number.el_prev_t2"),
        ent("input_text.el_plot"), ent("input_text.el_owner"), ent("input_text.el_membership"), ent("input_text.el_paid_to"),
        heading("Лічильник зараз", "mdi:meter-electric"),
        tile("sensor.dacha_meter_t1", "T1 (день)"), tile("sensor.dacha_meter_t2", "T2 (ніч)")]},
]}

# register dashboard
p = S + "lovelace_dashboards"
d = json.load(open(p)); shutil.copy(p, f"{p}.bak-{TS}")
items = [i for i in d["data"]["items"] if i["id"] != "tuya"]
items.append({"id": "tuya", "icon": "mdi:home-automation", "title": "Tuya", "url_path": "tuya",
              "require_admin": False, "show_in_sidebar": True, "mode": "storage"})
d["data"]["items"] = items; json.dump(d, open(p, "w"), ensure_ascii=False, indent=2)

# dashboard config
p = S + "lovelace.tuya"
try: shutil.copy(p, f"{p}.bak-{TS}")
except FileNotFoundError: pass
json.dump({"version": 1, "minor_version": 1, "key": "lovelace.tuya",
           "data": {"config": {"title": "Tuya", "views": [electric, heat, devices, sensors, bill]}}},
          open(p, "w"), ensure_ascii=False, indent=2)
print("tuya dashboard written, backups", TS)
