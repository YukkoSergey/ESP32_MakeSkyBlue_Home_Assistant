#include "web_ui.h"
#include "app_log.h"
#include "secrets.h"
#include "settings.h"
#include "alerts.h"
#if defined(ESP32)
#include <Update.h>
#endif

static CompatWebServer* s_server = nullptr;
static RelayController* s_relays = nullptr;
static web_ui::SystemFiller s_fillSystem;
static String           s_updateError;

static const char kPage[] PROGMEM = R"HTML(<!doctype html>
<html lang="uk"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Дача</title>
<style>
:root{--bg:#f6f7f9;--card:#fff;--fg:#1d2330;--mute:#6b7280;--line:#e3e6eb;--ok:#15803d;--bad:#b91c1c;--warn:#b45309;--acc:#2563eb}
@media(prefers-color-scheme:dark){:root{--bg:#12151b;--card:#1b1f27;--fg:#e6e8ec;--mute:#9aa3b2;--line:#2a303b;--ok:#4ade80;--bad:#f87171;--warn:#fbbf24;--acc:#60a5fa}}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--fg);font:14px/1.45 system-ui,sans-serif}
main{max-width:980px;margin:0 auto;padding:16px}
h1{font-size:20px;margin:4px 0 12px}h2{font-size:15px;margin:0 0 10px}
.grid{display:grid;gap:12px;grid-template-columns:repeat(auto-fit,minmax(280px,1fr))}
.card{background:var(--card);border:1px solid var(--line);border-radius:10px;padding:14px}
.kv{display:grid;grid-template-columns:auto 1fr;gap:4px 12px}.kv span:nth-child(odd){color:var(--mute)}
.on{color:var(--ok);font-weight:600}.off{color:var(--mute)}.bad{color:var(--bad);font-weight:600}.warn{color:var(--warn)}
button{font:inherit;padding:7px 12px;border-radius:7px;border:1px solid var(--line);background:var(--bg);color:var(--fg);cursor:pointer}
button.pri{background:var(--acc);border-color:var(--acc);color:#fff}button.sel{outline:2px solid var(--acc)}
.row{display:flex;flex-wrap:wrap;gap:8px;align-items:center}
table{border-collapse:collapse;width:100%}td,th{padding:4px 6px;text-align:left;border-bottom:1px solid var(--line)}
input[type=password],input[type=text],textarea{font:inherit;padding:5px;background:var(--bg);color:var(--fg);border:1px solid var(--line);border-radius:5px}
textarea{width:100%;font:12px/1.4 ui-monospace,Menlo,monospace;margin:4px 0 10px}
h3.sub{font-size:14px;margin:14px 0 6px}h3.sub:first-of-type{margin-top:0}details summary{cursor:pointer;margin:8px 0}
input[type=number]{width:72px;font:inherit;padding:4px;background:var(--bg);color:var(--fg);border:1px solid var(--line);border-radius:5px}
select{font:inherit;padding:4px;background:var(--bg);color:var(--fg);border:1px solid var(--line);border-radius:5px}
.scroll{overflow-x:auto}
pre{margin:0;height:340px;overflow:auto;background:var(--bg);border:1px solid var(--line);border-radius:7px;padding:8px;font:12px/1.4 ui-monospace,Menlo,monospace;white-space:pre-wrap}
#msg{min-height:1.4em}.mute{color:var(--mute)}
.why p{margin:4px 0}.alarm{background:#b91c1c;color:#fff;border-radius:10px;padding:12px 14px;margin-bottom:12px}.alarm b{font-size:15px}.alarm button{margin-top:8px;background:#fff;color:#b91c1c;border-color:#fff;font-weight:600}.alarm.acked{background:#92400e}.how h3{font-size:14px;margin:16px 0 4px}.how h3:first-child{margin-top:0}.how p,.how li{margin:3px 0}.how ul{margin:4px 0;padding-left:20px}
</style></head><body><main>
<h1>Дача <span class="mute" id="fw" style="font-size:13px;font-weight:400"></span></h1>
<div id="alarms"></div>
<div class="card why" style="margin-bottom:12px"><h2>Чому зараз так</h2><div id="why">завантаження…</div></div>
<div class="grid">
 <div class="card"><h2>Батарея і реле</h2><div class="kv" id="st"></div>
  <div class="row" style="margin-top:10px"><span class="mute">Важке навантаження:</span>
   <button data-h="auto">Авто</button><button data-h="on">Дозволити</button><button data-h="off">Заборонити</button>
  </div><div class="mute" style="margin-top:6px">«Дозволити» обходить пороги, але аварія за напругою і робота генератора все одно вимикають важке навантаження.</div></div>
 <div class="card"><h2>Генератор</h2><div class="kv" id="gen"></div>
  <div class="row" style="margin-top:10px">
   <button data-m="auto">Авто</button><button data-m="run">Запустити до мети</button><button data-m="off">Заборонити</button>
  </div><div class="mute" style="margin-top:6px">«Запустити до мети» працює, доки запас не дійде до «Генератор СТОП» поточного сезону.</div></div>
 <div class="card"><h2>Система</h2><div class="kv" id="sys"></div></div>
</div>
<div class="card" style="margin-top:12px"><h2>Налаштування</h2>
 <div class="row" style="margin-bottom:8px">
  <label>Ємність масиву, кВт·год <input type="number" step="0.1" id="cap"></label>
  <label>Сезон <select id="smode"><option value="0">авто (за датою)</option><option value="1">літо</option><option value="2">міжсезоння</option><option value="3">зима</option></select></label>
 </div>
 <div class="scroll"><table><thead><tr><th>Сезон</th><th>Важке ВВІМК &gt;</th><th>Важке ВИМК &lt;</th><th>Генератор авто</th><th>Генератор СТАРТ &lt;</th><th>Генератор СТОП &gt;</th></tr></thead><tbody id="prof"></tbody></table></div>
 <div class="mute" style="margin:6px 0">Усі пороги — запас енергії в кВт·год (SOC × ємність). Авто-сезони: зима 1.12–28.02, міжсезоння 1.03–30.04 і 15.10–30.11, літо 1.05–14.10.</div>
 <div class="row"><button class="pri" id="save">Зберегти</button><span id="msg"></span></div>
</div>
<div class="card" style="margin-top:12px"><h2>Сповіщення</h2>
 <h3 class="sub">Telegram</h3>
 <div id="tgState">завантаження…</div>
 <div id="tgForm" hidden><ol>
  <li>У Telegram відкрий <b>@BotFather</b>, надішли <code>/newbot</code>, дай боту назву та ім'я користувача (має закінчуватись на <code>bot</code>, напр. <code>dacha_yukko_bot</code>).</li>
  <li>BotFather відповість токеном виду <code>123456789:AA…</code>. Встав його сюди й збережи:</li></ol>
  <div class="row"><input id="tgToken" type="password" placeholder="123456789:AA..." style="flex:1;min-width:220px"><button class="pri" id="tgSave">Зберегти токен</button><span id="tgMsg"></span></div>
 </div>
 <div id="tgSeen"></div>
 <div id="tgRecent"></div>
 <h3 class="sub">Home Assistant (push на телефон)</h3>
 <div id="haState">завантаження…</div>
 <details><summary>Як підключити push через HA (Android, iPhone)</summary>
<p>Плата публікує кожну тривогу в топік <code>dacha_relays/alert</code> на брокері HA. Три автоматизації нижче роблять із цього push на телефон і пульс для сторожа HA.</p>
<ol>
<li>Встанови застосунок <b>Home Assistant</b> на телефон і увійди у свій HA. Телефон з'явиться в HA як пристрій.</li>
<li>Дізнайся назву сервісу телефона: HA → Інструменти розробника → Дії → почни вводити <code>notify.mobile_app_</code>. Це буде щось на зразок <code>notify.mobile_app_pixel_8</code>.</li>
<li>HA → Налаштування → Автоматизації → Створити → ⋮ → «Редагувати в YAML». Встав по черзі кожну автоматизацію і збережи. У другій заміни <code>notify.mobile_app_ТВІЙ_ТЕЛЕФОН</code> на свою назву.</li>
<li>Android: тривоги йдуть каналом <code>alarm_stream</code> і звучать як будильник навіть у режимі «Не турбувати». Інформаційні — каналом «dacha_info», звучання налаштуєш у Android.</li>
</ol>
<p><b>1. Пульс HA для плати</b> (без неї плата не знає, що HA живий):</p>
<textarea readonly rows="11">alias: Дача — пульс для ESP32
description: Щохвилини каже платі, що HA живий. Без пульсу 5 хв плата пише в Telegram.
trigger:
  - platform: time_pattern
    minutes: "/1"
action:
  - service: mqtt.publish
    data:
      topic: dacha_relays/ha_heartbeat
      payload: "ok"
mode: single</textarea>
<p><b>2. Тривоги плати → push на телефон</b>:</p>
<textarea readonly rows="27">alias: Дача — тривоги ESP32 на телефон
description: Тривоги повторюються, доки не натиснеш «Прийняв». Same tag = нове замінює старе.
trigger:
  - platform: mqtt
    topic: dacha_relays/alert
action:
  - variables:
      a: "{{ trigger.payload_json }}"
      urgent: "{{ a.level in ['alarm', 'repeat'] }}"
  - service: notify.mobile_app_ТВІЙ_ТЕЛЕФОН
    data:
      title: "{{ a.title }}"
      message: "{{ a.text if a.text else a.title }}"
      data:
        tag: "dacha_{{ a.key }}"
        ttl: 0
        priority: high
        channel: "{{ 'alarm_stream' if urgent else 'dacha_info' }}"
        actions: >-
          {{ [{'action': 'DACHA_ACK_' ~ a.key, 'title': 'Прийняв'}] if urgent else [] }}
        push:
          interruption-level: "{{ 'critical' if urgent else 'active' }}"
mode: queued</textarea>
<p><b>3. Кнопка «Прийняв» з телефона → плата</b>:</p>
<textarea readonly rows="13">alias: Дача — «Прийняв» з телефона
trigger:
  - platform: event
    event_type: mobile_app_notification_action
condition:
  - condition: template
    value_template: "{{ trigger.event.data.action.startswith('DACHA_ACK_') }}"
action:
  - service: mqtt.publish
    data:
      topic: dacha_relays/alert/ack
      payload: "{{ trigger.event.data.action[10:] }}"
mode: queued</textarea>
<p class="mute">Кнопка «Прийняв» з телефона спрацює тільки після TLS на брокері HA: без шифрування плата не приймає з HA жодних команд, бо будь-хто з перехопленим паролем зміг би глушити тривоги. До того приймай у Telegram або тут. Блок <code>push:</code> — для iPhone, Android його ігнорує.</p>
 </details>
</div>
<div class="card how" style="margin-top:12px"><h2>Як працює плата</h2><div id="how">завантаження…</div></div>
<div class="card" style="margin-top:12px"><h2>Лог</h2>
 <div class="row" style="margin-bottom:6px"><label><input type="checkbox" id="follow" checked> автопрокрутка</label><button id="clr">Очистити екран</button></div>
 <pre id="log"></pre></div>
<div class="card" style="margin-top:12px"><h2>Сервіс</h2>
 <div class="row"><input type="file" id="fwfile" accept=".bin"><button id="upl">Залити прошивку</button><span id="upmsg"></span></div>
 <div class="row" style="margin-top:10px"><button id="reboot">Перезавантажити</button><button id="reset">Скинути стан реле</button></div>
</div>
</main><script>
const $=id=>document.getElementById(id);
const SEASONS={summer:'літо',shoulder:'міжсезоння',winter:'зима'};
const MODES={auto:'авто',run:'запуск до мети',off:'заборонено'};
const onoff=(v,a='УВІМК',b='вимк')=>`<b class="${v?'on':'off'}">${v?a:b}</b>`;
function kv(el,rows){el.innerHTML=rows.map(([k,v])=>`<span>${k}</span><span>${v}</span>`).join('')}
function age(s){return s<0?'—':s<90?s+' с':Math.round(s/60)+' хв'}
function up(s){const d=Math.floor(s/86400),h=Math.floor(s%86400/3600),m=Math.floor(s%3600/60);return (d?d+' д ':'')+h+' год '+m+' хв'}
async function api(p,o){const r=await fetch(p,o);if(!r.ok)throw new Error(await r.text()||r.status);return r}
async function status(){
 try{const j=await (await api('/api/status')).json();const r=j.relays,s=j.system,t=r.thresholds;
  $('fw').textContent='· '+s.build.replace(/^(NodeMCU )?/, m=>m?'плата NodeMCU, ':'плата ESP32, ')+' (час збірки прошивки)';
  kv($('st'),[
   ['SOC',r.soc<0?'—':r.soc+' %'],['Запас',r.reserve_kwh<0?'—':r.reserve_kwh+' / '+r.capacity_kwh+' кВт·год'],
   ['Дані SA',r.soc_fresh?`<span class="on">свіжі</span> (${age(r.soc_age_s)})`:`<span class="bad">немає (${age(r.soc_age_s)}) — економ</span>`],
   ['Сезон',SEASONS[r.season]+(r.season_mode=='auto'?' (авто)':' (вручну)')],
   ['Важке навантаження',onoff(r.heavy_load,'ДОЗВОЛЕНО','економ')+(r.heavy_mode=='auto'?' (авто)':r.heavy_mode=='on'?' <span class="warn">(дозволено вручну)</span>':' <span class="warn">(заборонено вручну)</span>')],
   ['Пороги важкого',`ВВІМК &gt; ${t.heavy_on_kwh} · ВИМК &lt; ${t.heavy_off_kwh}`],
   ['Батарея (BMS)',r.battery_w+' Вт'],['Напруга батареї (інвертор)',r.inv_battery_v<0?'—':r.inv_battery_v+' В'+(r.volt_emergency?' <span class="bad">АВАРІЯ</span>':'')],
   ['Мережа',r.grid_v<0?'—':(r.grid_v>=100?'є, '+r.grid_v+' В':'немає')],
   ['Перевірка застигання',r.frozen_paused?'на паузі (генератор)':`ніч без мережі ${r.frozen_night_wh}/1000 Вт·год · інвертор ${r.frozen_inv_wh}/1500 Вт·год`+(r.night?' · ніч':'')]]);
  kv($('gen'),[
   ['Реле (команда)',onoff(r.generator,'ЗАМКНУТЕ','розімкнуте')],
   ['Генератор (контакт)',r.gen_fb_enabled?(r.gen_fb_running?'<b class="on">ПРАЦЮЄ</b>':'стоїть')+(r.gen_no_start?' <span class="bad">не завівся!</span>':''):'<span class="mute">зворотний зв\'язок вимкнено</span> <button id="fbOn">увімкнути</button>'],
   ['Режим',MODES[r.gen_mode]],
   ['Авто в цьому сезоні',t.gen_auto?`СТАРТ &lt; ${t.gen_start_kwh} · СТОП &gt; ${t.gen_stop_kwh}`:'вимкнено'],
   ['Мета ручного запуску',t.gen_stop_kwh+' кВт·год'],
   ['Працює',r.generator?r.gen_on_min+' хв':'—'],['Зарядив за сесію',r.gen_session_kwh+' кВт·год'],
   ['До мети',r.gen_eta_h<0?'—':'~'+r.gen_eta_h+' год']]);
  document.querySelectorAll('[data-m]').forEach(b=>b.classList.toggle('sel',b.dataset.m==r.gen_mode));
  document.querySelectorAll('[data-h]').forEach(b=>b.classList.toggle('sel',b.dataset.h==r.heavy_mode));
  if($('fbOn'))$('fbOn').onclick=async()=>{if(confirm('Увімкнути зворотний зв\'язок від генератора? Контакт контактора генератора має бути підключений між D5 і G.')){await api('/api/gen_feedback',{method:'POST',body:'on'});status()}};
  lastStatus=j;renderAlarms(j.alerts);renderWhy(r);renderHow();
  kv($('sys'),[
   ['Аптайм',up(s.uptime_s)],['Час',s.local_time||'не синхронізовано'],['WiFi',`${s.wifi_ssid} (${s.wifi_rssi} dBm), ${s.ip}`],
   ['MQTT LAN',onoff(s.mqtt_lan,'ok','немає')],['MQTT HA',onoff(s.mqtt_ha,'ok','немає')+(s.ha_tls?' TLS':' <span class="warn">без TLS</span>')],
   ...(s.tuya_last_ok_s===undefined?[]:[['Tuya',s.tuya_last_ok_s<0?'ще не було':'остання успішна '+age(s.tuya_last_ok_s)+' тому']]),
   ['Telegram',j.alerts.telegram?(j.alerts.queue?`<span class="warn">у черзі ${j.alerts.queue}</span> ${j.alerts.last_error}`:'<span class="on">ok</span>'+(j.alerts.last_ok_s>=0?', останнє '+age(j.alerts.last_ok_s)+' тому':'')):'<span class="warn">не налаштовано</span>'+(j.alerts.telegram_token?' (є токен, немає chat id)':'')],
   ['Heap',`${s.heap} (мін ${s.heap_min})`],['Причина старту',s.reset_reason]]);
 }catch(e){kv($('st'),[['Помилка',e.message]])}
}
let logPos=0;
async function log(){
 try{const r=await api('/api/log?since='+logPos);logPos=+r.headers.get('X-Log-Pos');const t=await r.text();
  if(t){const el=$('log');el.textContent+=t;if(el.textContent.length>200000)el.textContent=el.textContent.slice(-150000);if($('follow').checked)el.scrollTop=el.scrollHeight}}catch(e){}
}
async function loadSettings(){
 const j=await (await api('/api/settings')).json();lastSettings=j;renderHow();$('cap').value=j.capacity_kwh;$('smode').value=j.season_mode;
 $('prof').innerHTML=j.profiles.map((p,i)=>`<tr data-i="${i}"><td>${['літо','міжсезоння','зима'][i]}</td>
  <td><input type="number" step="0.1" data-k="heavy_on_kwh" value="${p.heavy_on_kwh}"></td>
  <td><input type="number" step="0.1" data-k="heavy_off_kwh" value="${p.heavy_off_kwh}"></td>
  <td><input type="checkbox" data-k="gen_auto" ${p.gen_auto?'checked':''}></td>
  <td><input type="number" step="0.1" data-k="gen_start_kwh" value="${p.gen_start_kwh}"></td>
  <td><input type="number" step="0.1" data-k="gen_stop_kwh" value="${p.gen_stop_kwh}"></td></tr>`).join('');
}
let lastStatus=null,lastSettings=null;
const SN=['літо','міжсезоння','зима'];
const kwh=v=>(+v).toFixed(1).replace(/\.0$/,'')+' кВт·год';
function pct(v,cap){return cap>0?Math.round(v*100/cap)+' %':'?'}
function renderAlarms(a){
 $('alarms').innerHTML=(a.active||[]).map(x=>`<div class="alarm${x.acked?' acked':''}"><b>🚨 ${x.title}</b> <span>(${x.since_min} хв${x.acked?', прийнято':''})</span><div style="white-space:pre-wrap;margin-top:4px">${x.detail}</div>${x.acked?'':`<button data-ack="${x.key}">Прийняв</button>`}</div>`).join('');
 document.querySelectorAll('[data-ack]').forEach(b=>b.onclick=async()=>{await api('/api/alerts/ack',{method:'POST',body:b.dataset.ack}).catch(()=>{});status()});
}
function renderWhy(r){
 const t=r.thresholds,res=r.reserve_kwh,cap=r.capacity_kwh,sea=SEASONS[r.season];
 const lines=[];
 if(r.volt_emergency)lines.push(`<b class="bad">Аварія за напругою</b>: батарея ${r.inv_battery_v} В, нижче 47 В понад 5 хв. Вона майже порожня, хоч би що показував SOC. Важке навантаження вимкнено, генератор запущено (якщо не заборонений).`);
 if(r.gen_no_start)lines.push(`<b class="bad">Генератор не працює</b>, хоча реле замкнуте понад 3 хв: контакт зворотного зв'язку не замкнувся. Перевір паливо і сам генератор.`);
 if(r.gen_fb_enabled&&r.gen_fb_running&&!r.generator)lines.push(`<b class="warn">Генератор працює</b> (контакт замкнутий), хоча плата його не запускала. Важке навантаження вимкнене, поки він працює.`);
 if(r.heavy_mode=='off')lines.push(`<b class="warn">Важке навантаження заборонене вручну.</b> Пороги не діють, доки не натиснеш «Авто».`);
 if(r.heavy_mode=='on')lines.push(`<b class="warn">Важке навантаження дозволене вручну</b>, незалежно від порогів`+(r.heavy_load?'.':', але зараз його вимкнув захист (аварія за напругою або працює генератор).'));
 if(r.charger_detected&&!r.generator)lines.push(`<b class="warn">Працює зарядник генератора</b>, хоча плата його не запускала (BMS бачить на ~${r.charger_w} Вт більше заряду, ніж інвертор). Генератор завели вручну або його власний контролер.`);
 if((r.generator||r.charger_detected)&&r.soc_fresh&&!r.soc_frozen&&!r.volt_emergency)lines.push(`<b>Важке навантаження вимкнене, поки працює генератор</b>, за будь-якого SOC: зарядник дає ~1 кВт і не потягне важке навантаження.`);
 if(r.soc_frozen){
  lines.push(`<b class="bad">SOC застиг на ${r.soc} %</b> (${r.frozen_reason}). Плата йому не довіряє, тому важке навантаження <b>заборонене</b>, а генератор лишається як був. Повернеться до норми, щойно SOC почне змінюватись.`);
 }else if(!r.soc_fresh){
  lines.push(`<b class="bad">Немає свіжих даних від Solar Assistant</b> (${age(r.soc_age_s)}). Тому важке навантаження <b>заборонене</b> (економ-режим), доки дані не повернуться.`);
 }else if(r.heavy_mode!='auto'){
 }else if(r.heavy_load){
  lines.push(`<b class="on">Важке навантаження дозволене</b>: запас ${kwh(res)} (${r.soc} %). Вимкнеться, коли запас впаде нижче ${kwh(t.heavy_off_kwh)} (поріг сезону «${sea}»).`);
 }else if(res>t.heavy_on_kwh){
  lines.push(`<b>Важке навантаження заборонене</b>, хоча запас ${kwh(res)} вищий за поріг. Увімкнеться з наступним значенням SOC.`);
 }else{
  lines.push(`<b>Важке навантаження заборонене</b> (економ): запас ${kwh(res)} (${r.soc} %). Дозволиться, коли запас підніметься вище ${kwh(t.heavy_on_kwh)} (поріг сезону «${sea}»).`+
   (res>=t.heavy_off_kwh?` Зараз запас між порогами ВИМК ${kwh(t.heavy_off_kwh)} і ВВІМК ${kwh(t.heavy_on_kwh)}: це гістерезис, реле лишається як було.`:''));
 }
 if(r.gen_mode=='off'){
  lines.push(`<b>Генератор заборонений вручну</b>. Не запуститься, доки не натиснеш «Авто».`);
 }else if(r.gen_mode=='run'){
  lines.push(`<b class="on">Генератор запущений вручну</b> і працюватиме, доки запас не дійде до ${kwh(t.gen_stop_kwh)}`+(r.soc_fresh?`, зараз ${kwh(res)}.`:'. Даних від SA немає, тому він працює далі, доки є паливо.'));
 }else if(r.generator){
  const trusted=r.soc_fresh&&!r.soc_frozen;
  lines.push(`<b class="on">Генератор працює</b> (авто). `+(trusted?`Зупиниться, коли запас перевищить ${kwh(t.gen_stop_kwh)}, зараз ${kwh(res)}.`:`SOC зараз недостовірний, тому генератор працює далі, доки SOC не стане достовірним і не перевищить ${kwh(t.gen_stop_kwh)}, або доки є паливо.`));
 }else if(!t.gen_auto){
  lines.push(`<b>Генератор стоїть</b>. Автозапуск у сезоні «${sea}» вимкнений, запустити можна тільки кнопкою.`);
 }else if(!r.soc_fresh||r.soc_frozen){
  lines.push(`<b>Генератор стоїть</b>. SOC недостовірний, тому за порогами він не запуститься. Запустить його лише аварія за напругою (батарея нижче 47 В) або кнопка.`);
 }else{
  lines.push(`<b>Генератор стоїть</b>. Запуститься автоматично, коли запас впаде нижче ${kwh(t.gen_start_kwh)} (${pct(t.gen_start_kwh,cap)}), зараз ${kwh(res)}.`);
 }
 $('why').innerHTML=lines.map(l=>'<p>'+l+'</p>').join('');
}
function renderHow(){
 if(!lastStatus||!lastSettings)return;
 const r=lastStatus.relays,s=lastStatus.system,cfg=lastSettings,cap=cfg.capacity_kwh;
 const rows=cfg.profiles.map((p,i)=>`<tr${SN[i]==SEASONS[r.season]?' style="font-weight:600"':''}><td>${SN[i]}${SN[i]==SEASONS[r.season]?' ← зараз':''}</td>
  <td>${kwh(p.heavy_on_kwh)} (${pct(p.heavy_on_kwh,cap)})</td><td>${kwh(p.heavy_off_kwh)} (${pct(p.heavy_off_kwh,cap)})</td>
  <td>${p.gen_auto?kwh(p.gen_start_kwh)+' ('+pct(p.gen_start_kwh,cap)+')':'вимкнено'}</td><td>${kwh(p.gen_stop_kwh)} (${pct(p.gen_stop_kwh,cap)})</td></tr>`).join('');
 $('how').innerHTML=`
<h3>Що робить плата</h3>
<ul><li><b>Керує двома реле</b> за запасом енергії в батареї: дозвіл важкого навантаження і генератор. Це головна задача, і вона не залежить від інтернету.</li>
<li>Пересилає дані трьох контролерів MakeSkyBlue MPPT (з Tuya Cloud) у Home Assistant для картки Solar_Manager.</li>
<li>Пересилає показники Solar Assistant у віртуальний пристрій Tuya (MUST), щоб їх було видно в SmartLife.</li></ul>
<h3>Підключення</h3>
<ul><li><b>${r.pin_heavy} → реле 1 (IN1)</b>: живить БЖ 24 В. Від нього сигнальна лінія йде до контактора в будинку. Є напруга, отже важке навантаження (бойлер, тепла підлога, кондиціонер / тепловий насос) може працювати від батарей.</li>
<li><b>${r.pin_gen} → реле 2 (IN2)</b>: сухі контакти АВР генератора. Замкнуто, отже генератор працює.</li>
<li>${r.relay_active_high?'Модуль реле в режимі <b>High level trigger</b> (перемички S1 і S2 на «High»): HIGH вмикає реле, LOW або обірваний провід вимикає. Під час старту, перезавантаження чи без живлення плати обидва реле вимкнені.':'Реле керуються інверсно: LOW вмикає. Під час старту і перезавантаження обидва вимкнені.'}</li>
<li>Живлення модуля: ${r.relay_active_high?'<b>DC+ → VU</b> (5 В з USB), DC− → G. Не VIN: на цій NodeMCU VIN — лише вхід живлення, при живленні від USB на ньому 0 В. Блок живлення має давати 5 В (на VU 4.7–5.2 В); старий блок давав ~8 В, це забагато для реле.':'VCC → 5 В, GND → GND.'}</li></ul>
<h3>Запас енергії і пороги</h3>
<p>Запас = SOC × ємність масиву. Зараз ємність <b>${kwh(cap)}</b>. Пороги задані в кВт·год, а не у відсотках: скільки енергії треба, щоб пережити ніч, залежить від економного споживання і кількості годин, а не від розміру масиву. Коли додаєш батарею, змінюй тільки ємність у «Налаштуваннях».</p>
<div class="scroll"><table><thead><tr><th>Сезон</th><th>Важке ВВІМК, якщо запас &gt;</th><th>Важке ВИМК, якщо &lt;</th><th>Генератор СТАРТ, якщо &lt;</th><th>Генератор СТОП, якщо &gt;</th></tr></thead><tbody>${rows}</tbody></table></div>
<p class="mute">Відсотки в дужках розраховані для поточної ємності. Між порогами ВВІМК і ВИМК реле лишається в тому стані, в якому було (гістерезис), щоб не клацало туди-сюди.</p>
<h3>Сезони</h3>
<p>За датою (київський час): <b>зима</b> 1.12–28.02, <b>міжсезоння</b> 1.03–30.04 і 15.10–30.11, <b>літо</b> 1.05–14.10. Сезон можна зафіксувати вручну в «Налаштуваннях». Зараз: ${SEASONS[r.season]} (${r.season_mode=='auto'?'за датою':'вручну'}). Якщо плата ще не знає дату (немає інтернету після старту), вона бере зимовий профіль як найобережніший.</p>
<h3>Генератор</h3>
<ul><li>Схема: генератор → зарядник LiFePO4 (21 А) → батарея. Інвертор про генератор не знає, тому окремого сигналу «генератор працює» немає.</li>
<li><b>Зворотний зв'язок</b> (${r.gen_fb_enabled?'<span class="on">увімкнено</span>':'вимкнено'}): допоміжний контакт контактора генератора між <b>D5 і G</b>; замкнутий = генератор справді працює. Тоді плата знає реальний стан генератора, а якщо реле замкнуте 3 хв, а контакт ні, — тривога «генератор не завівся». Підключати тільки сухий контакт, не 230 В.</li>
<li><b>Поки працює генератор, важке навантаження вимкнене за будь-якого SOC</b>: зарядник дає ~1 кВт, і важке навантаження розряджало б батарею, яку він заряджає. «Працює» — це або плата замкнула його реле, або вночі в даних видно зарядник (BMS показує заряд на 300+ Вт більший, ніж інвертор, протягом 2 хв), навіть якщо генератор завели вручну. Удень так розпізнати не можна: контролери MakeSkyBlue теж заряджають батарею повз інвертор.</li>
<li>Важке навантаження теж має режими: <b>Авто</b> (за порогами сезону), <b>Дозволити</b> (незалежно від порогів), <b>Заборонити</b> (вимкнено, напр. на час ремонту). Аварія за напругою і робота генератора вимикають його в будь-якому режимі. Після перезавантаження зберігається тільки «Заборонити».</li>
<li>Режими генератора: <b>Авто</b> (за порогами сезону); <b>Запустити до мети</b> (працює, доки запас не дійде до «Генератор СТОП» поточного сезону, потім сам повертається в Авто); <b>Заборонити</b> (не запуститься, доки не повернеш Авто).</li>
<li>За годину роботи генератор у середньому додає ~0.7 кВт·год (за історією). «Зарядив за сесію» і «До мети» — приблизні оцінки з балансу енергії батареї.</li></ul>
<h3>Безпека: що буває, коли щось ламається</h3>
<ul><li><b>Немає даних від Solar Assistant 3 хв</b> (впав SA, брокер або WiFi): важке навантаження вимикається (економ). Генератор, що працює, працює далі, доки є паливо. Генератор, що стоїть, наосліп не запускається.</li>
<li><b>Немає інтернету</b>: усе працює локально, бо SA і брокер у локальній мережі. Пропадають тільки HA і Tuya.</li>
<li><b>Неправильні дані</b> (<code>unavailable</code>, SOC поза 0–100) ігноруються.</li>
<li><b>Дані приходять, але SOC застиг</b>: див. «Захист від застиглих даних» нижче.</li>
<li><b>Перезавантаження</b>: плата вирішує з нуля. Обидва реле вимкнені, доки не прийде свіжий SOC; далі важке навантаження вмикається тільки вище порогу ВВІМК, генератор — тільки нижче СТАРТ (між порогами реле лишається вимкненим). Зберігається лише ручне «Заборонити»; «Дозволити» і «Запустити до мети» скидаються в «Авто».</li>
<li><b>Плата зависла</b>: watchdog перезавантажить її через 60 с. Поки вона перезавантажується, обидва реле відпущені.</li>
<li><b>Плата без живлення</b>: обидва реле відпущені, отже економ-режим і генератор стоїть.</li></ul>
<h3>Захист від застиглих даних</h3>
<p>Буває, що Solar Assistant втрачає зв'язок з Pylontech (USB / консоль), але продовжує публікувати останній SOC. За історією 2024–2026 таке траплялося щонайменше 7 разів: від години до 58 діб (травень–липень 2026 SOC «стояв» на 100 %). Тому плата звіряє SOC з тим, що незалежно вимірює інвертор:</p>
<ul><li><b>Вночі без мережі</b> все споживання йде з батареї (контролери MakeSkyBlue вночі не працюють). Якщо будинок спожив 1 кВт·год, а SOC не зрушив, SOC застиг. Нормально він змінюється вже після ~0.1 кВт·год. Діє при будь-якому SOC, включно зі 100 %.</li>
<li><b>Нижче 98 %</b>: якщо за струмом і напругою інвертора через батарею пройшло 1.5 кВт·год, а SOC стоїть, SOC застиг. Вище 98 % це правило вимкнене: там інвертор показує фіктивний розряд ~100 Вт.</li>
<li>Поки працює генератор і ще 30 хв після, обидва правила на паузі: зарядник додає енергію, якої інвертор не бачить.</li>
<li>Застиглий SOC = те саме, що відсутність даних: важке навантаження вимкнене, генератор як був, плюс тривога в Telegram. Знімається, щойно SOC зміниться.</li></ul>
<h3>Аварія за напругою</h3>
<p>Напруга батареї з інвертора нижче <b>47.0 В понад 5 хв</b> означає, що батарея майже порожня, незалежно від SOC (за історією таке було лише при SOC 4 % і 11 %; інвертор сам вимикається при 45.0 В). Тоді: важке навантаження вимкнено, <b>генератор запускається в будь-якому сезоні</b>, якщо його не заборонено вручну, і йде тривога. Генератор зупиниться за звичайним порогом СТОП, коли SOC знову стане достовірним.</p>
<h3>Сповіщення</h3>
<ul><li>Тривоги (немає даних SA, SOC застиг, аварія за напругою) йдуть у Telegram і <b>повторюються кожні 10 хв</b>, доки не натиснеш «Прийняв» у повідомленні або тут, на сторінці. Коли проблема минає, приходить «✅ минуло».</li>
<li>Без повтору: запуск і зупинка генератора, перезавантаження плати (з причиною).</li>
<li>Немає інтернету: повідомлення чекають у черзі й підуть, щойно зв'язок з'явиться. Команда <code>/status</code> боту відповідає поточним станом.</li>
<li>Ті самі повідомлення йдуть і в Home Assistant (топік <code>dacha_relays/alert</code>), звідки автоматизація робить push на телефон. Два незалежні шляхи: якщо впаде один, лишиться другий.</li>
<li><b>Сторож HA</b>: HA щохвилини шле платі пульс. Якщо пульсу немає 5 хв, HA повідомив offline або брокер HA недоступний 5 хв, плата пише про це в Telegram.</li>
<li>Налаштування: картка «Сповіщення» вище. Зараз Telegram: ${lastStatus.alerts.telegram?'<span class="on">працює</span>':'<span class="warn">не налаштовано</span>'}.</li></ul>
<h3>Сервіс на VPS (dacha-bridge)</h3>
<ul><li>Працює в k3s поруч із HA (namespace <code>homeassistant</code>), окремо від нього.</li>
<li><b>MakeSkyBlue → HA</b>: опитує три MPPT у Tuya Cloud і публікує кадри Solar_Manager (<code>msb1/2/3/notify</code>) — картка MPPT у HA.</li>
<li><b>Solar Assistant → Tuya</b>: ця плата пересилає 6 значень SA у <code>dacha_relays/sa/*</code>, сервіс віддає їх віртуальному пристрою MUST (SmartLife).</li>
<li><b>Telegram</b> (@dacha_yukko_bot): тривоги плати з кнопкою «Прийняв», /status. Пише сам, якщо <b>плата мовчить</b> 5 хв або <b>HA не відповідає</b>.</li>
<li><b>Підписані команди</b>: «Прийняв» з Telegram приходить у <code>dacha_relays/cmd</code> з підписом HMAC-SHA256 (ключ CMD_SECRET у secrets.h і в k8s Secret). Підробити чи повторити таку команду без ключа неможливо, тому TLS для неї не потрібен.</li></ul>
<h3>Що працює поза платою</h3>
<ul><li><b>Річне реле часу</b> в щиті влітку вимикає мережу повністю: будинок працює від сонця і батарей.</li>
<li><b>Реле напруги батареї</b> вмикає мережу в обхід реле часу, якщо в другій половині дня напруга низька (немає сонця, насувається шторм). Воно незалежне від плати й ніколи не підводило.</li>
<li><b>Перемикач теплового насоса / кондиціонера</b> в щиті дозволяє ввімкнути їх в обхід логіки контактора. Це ручне рішення, плата про нього не знає.</li></ul>
<h3>Home Assistant</h3>
<p>Сутності пристрою «Dacha relays» з'являються в HA автоматично (MQTT discovery). Команди з HA (режим генератора, сезон) плата приймає <b>тільки через зашифроване з'єднання (TLS)</b>, бо інакше пароль брокера йде відкритим текстом і хтось інший міг би запустити генератор. Зараз: ${s.ha_tls?'<span class="on">TLS є, команди з HA приймаються</span>':'<span class="warn">TLS немає, HA тільки показує стан</span>'}.</p>
<h3>Звідки взялися пороги</h3>
<p>З аналізу історії Solar Assistant (листопад 2024 – жовтень 2026). До січня 2026 ємність була 4.8 кВт·год (одна US5000), потім 9.6 (додано 2×US2000). Економне споживання вночі без мережі: взимку 150 Вт медіана, 390 Вт у 90 % годин; влітку 135 / 385 Вт. Зимові відключення: зазвичай до 6 год, найдовше 36 год. Звідси взимку важке навантаження вимикається на 6.5 кВт·год: це довга ніч в економ-режимі (14 год × 390 Вт ≈ 5.5 кВт·год) плюс запас. Влітку мережу повертає реле напруги, тому генератор лише аварійний.</p>
<h3>Обслуговування</h3>
<ul><li>Оновити прошивку: у «Сервісі» нижче виберіть <code>firmware.bin</code>, або з Mac командою <code>pio run -e ota -t upload</code> (пароль OTA в <code>include/secrets.h</code>).</li>
<li>Паролі (WiFi, MQTT, Tuya, цей веб, OTA) лежать у <code>include/secrets.h</code> проєкту на Mac. У git цей файл не потрапляє.</li>
<li>Код: проєкт <code>esp32-dacha</code> (GitHub YukkoSergey/ESP32_MakeSkyBlue_Home_Assistant), опис у README.md.</li></ul>`;
}
function esc(s){return String(s==null?'':s).replace(/[&<>"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]))}
function ts(t){if(!t||t<1700000000)return'';return new Date(t*1000).toLocaleString('uk-UA',{day:'2-digit',month:'2-digit',hour:'2-digit',minute:'2-digit'})}
async function loadNotify(){try{renderNotify(await (await api('/api/notify')).json())}catch(e){}}
async function post(p,body){const r=await fetch(p,{method:'POST',body:body||''});if(!r.ok)throw new Error(await r.text());}
function renderNotify(j){
 const t=j.telegram,h=j.ha;let st='';
 if(t.unsupported){st='<p><span class="warn">Напряму з цієї плати — ні.</span> '+esc(t.unsupported)+'</p>';}
 else if(!t.token_set){st='<p><span class="warn">Не налаштовано.</span> Два кроки нижче, і тривоги підуть у Telegram.</p>';}
 else if(!t.chat){st=`<p>Бот ${t.bot?'<b>@'+esc(t.bot)+'</b>':'(перевіряю токен…)'}, токен ${esc(t.token_hint)}. ${t.last_error?'<span class="bad">'+esc(t.last_error)+'</span>':''}</p>
  <p><b>Крок 3.</b> Відкрий ${t.bot?'<b>t.me/'+esc(t.bot)+'</b>':'свого бота'} і надішли <code>/start</code>. Для групи: додай бота в групу й напиши там будь-що. Чат з'явиться нижче за кілька секунд, натисни «Використати».</p>
  <div class="row"><button id="tgReset">Інший токен</button></div>`;}
 else{st=`<p><span class="on">✅ Працює</span>: бот <b>@${esc(t.bot||'?')}</b> → чат <code>${esc(t.chat)}</code>. ${t.last_ok_s>=0?'Останнє відправлення '+age(t.last_ok_s)+' тому.':'Ще нічого не відправлено.'} ${t.queue?'<span class="warn">У черзі: '+t.queue+'.</span>':''} ${t.last_error?'<span class="bad">'+esc(t.last_error)+'</span>':''}</p>
  <div class="row"><button id="tgTest">Надіслати тест</button><button id="tgReset">Від'єднати бота</button></div>`;}
 $('tgState').innerHTML=st;$('tgForm').hidden=t.token_set||!!t.unsupported;
 $('tgSeen').innerHTML=(t.token_set&&t.seen.length)?'<p style="margin-top:8px">Чати, що писали боту:</p>'+t.seen.map(c=>`<div class="row" style="margin:4px 0"><button data-chat="${esc(c.id)}">Використати</button><span><b>${esc(c.name.trim()||c.id)}</b> <span class="mute">${esc(c.id)} · «${esc(c.text)}» · ${ts(c.when)}</span></span></div>`).join(''):'';
 $('tgRecent').innerHTML=t.recent.length?'<p class="mute" style="margin:10px 0 4px">Останні повідомлення в Telegram:</p><div class="scroll"><table>'+t.recent.map(m=>`<tr><td class="mute" style="white-space:nowrap">${ts(m.when)}</td><td>${m.ok?'✅':'❌'}</td><td style="white-space:pre-wrap">${esc(m.text)}</td></tr>`).join('')+'</table></div>':'';
 if($('tgTest'))$('tgTest').onclick=()=>post('/api/telegram/test').then(loadNotify);
 if($('tgReset'))$('tgReset').onclick=()=>{if(confirm('Від\'єднати бота? Тривоги перестануть іти в Telegram.'))post('/api/telegram/token','').then(loadNotify)};
 document.querySelectorAll('[data-chat]').forEach(b=>b.onclick=()=>post('/api/telegram/chat',b.dataset.chat).then(loadNotify));
 $('haState').innerHTML=`<p>MQTT-брокер HA: ${h.mqtt?'<span class="on">підключено</span>':'<span class="bad">немає зв\'язку</span>'}${h.tls?' (TLS)':' <span class="warn">(без TLS: HA тільки отримує, команди й «Прийняв» з HA не приймаються)</span>'}.<br>
  Пульс від HA: ${h.heartbeat_seen?(h.heartbeat_age_s<300?'<span class="on">є</span>, '+age(h.heartbeat_age_s)+' тому':'<span class="bad">немає '+age(h.heartbeat_age_s)+'</span>'):'<span class="warn">ще не було</span>: додай автоматизацію 1 нижче, тоді плата повідомить у Telegram, якщо HA зависне'}${h.ha_offline?' · <span class="bad">HA каже offline</span>':''}.
  ${h.pending?'<br><span class="warn">Чекають відправки в HA: '+h.pending+'</span>':''}</p>`;
}
$('tgSave').onclick=async()=>{const v=$('tgToken').value.trim();if(!v)return;try{await post('/api/telegram/token',v);$('tgToken').value='';$('tgMsg').textContent='';loadNotify()}catch(e){$('tgMsg').innerHTML='<span class="bad">'+esc(e.message)+'</span>'}};
document.querySelectorAll('textarea[readonly]').forEach(t=>t.onclick=()=>t.select());
$('save').onclick=async()=>{
 const body={capacity_kwh:+$('cap').value,season_mode:+$('smode').value,profiles:[...$('prof').children].map(tr=>{const o={};
  tr.querySelectorAll('input').forEach(i=>o[i.dataset.k]=i.type=='checkbox'?i.checked:+i.value);return o})};
 try{await api('/api/settings',{method:'POST',body:JSON.stringify(body)});$('msg').innerHTML='<span class="on">збережено</span>';await loadSettings();status()}
 catch(e){$('msg').innerHTML='<span class="bad">'+e.message+'</span>'}
};
document.querySelectorAll('[data-h]').forEach(b=>b.onclick=async()=>{
 if(b.dataset.h=='on'&&!confirm('Дозволити важке навантаження незалежно від порогів?'))return;
 try{await api('/api/heavy_mode',{method:'POST',body:b.dataset.h});status()}catch(e){alert(e.message)}});
document.querySelectorAll('[data-m]').forEach(b=>b.onclick=async()=>{
 if(b.dataset.m!='auto'&&!confirm(b.textContent+'?'))return;
 try{await api('/api/gen_mode',{method:'POST',body:b.dataset.m});status()}catch(e){alert(e.message)}});
$('reboot').onclick=async()=>{if(confirm('Перезавантажити плату?')){await api('/api/reboot',{method:'POST'}).catch(()=>{});logPos=0}};
$('reset').onclick=async()=>{if(confirm('Вимкнути обидва реле і поставити генератор в авто?')){await api('/api/reset_state',{method:'POST'});status()}};
$('clr').onclick=()=>$('log').textContent='';
$('upl').onclick=()=>{const f=$('fwfile').files[0];if(!f)return;if(!confirm('Залити '+f.name+' ('+Math.round(f.size/1024)+' КБ)?'))return;
 const fd=new FormData();fd.append('firmware',f,f.name);const x=new XMLHttpRequest();x.open('POST','/update');
 x.upload.onprogress=e=>$('upmsg').textContent=Math.round(e.loaded*100/e.total)+' %';
 x.onload=()=>{$('upmsg').innerHTML=x.status==200?'<span class="on">готово, плата перезавантажується</span>':'<span class="bad">'+x.responseText+'</span>';logPos=0};
 x.onerror=()=>$('upmsg').innerHTML='<span class="bad">помилка з\'єднання</span>';x.send(fd)};
status();loadSettings();log();loadNotify();setInterval(status,3000);setInterval(log,2000);setInterval(loadNotify,5000);
</script></body></html>
)HTML";

static bool authed() {
    if (s_server->authenticate(WEB_USERNAME, WEB_PASSWORD)) return true;
    s_server->requestAuthentication(BASIC_AUTH, "dacha-esp32");
    return false;
}

// Floats go out as doubles (9.6f -> 9.600000381); round for display.
static double r2(float v) { return round(v * 100.0) / 100.0; }

static void sendJson(JsonDocument& doc) {
    String out;
    serializeJson(doc, out);
    s_server->send(200, "application/json", out);
}

static void handleRoot() {
    if (s_server->header("Accept").indexOf("text/html") < 0) {
        s_server->send(200, "application/json", s_relays->legacyStatusJson());
        return;
    }
    if (!authed()) return;
    s_server->send_P(200, "text/html; charset=utf-8", kPage);
}

static void handleStatus() {
    if (!authed()) return;
    DynamicJsonDocument doc(4096);
    s_relays->fillStatus(doc.createNestedObject("relays"));
    s_fillSystem(doc.createNestedObject("system"));
    alerts.fillStatus(doc.createNestedObject("alerts"));
    sendJson(doc);
}

static void handleLog() {
    if (!authed()) return;
    uint32_t since = strtoul(s_server->arg("since").c_str(), nullptr, 10);
    String text;
    uint32_t end = Log.readSince(since, text);
    s_server->sendHeader("X-Log-Pos", String(end));
    s_server->sendHeader("Cache-Control", "no-store");
    s_server->send(200, "text/plain; charset=utf-8", text);
}

static void handleGetSettings() {
    if (!authed()) return;
    const Settings& s = s_relays->currentSettings();
    DynamicJsonDocument doc(1024);
    doc["capacity_kwh"] = r2(s.capacityKwh);
    doc["season_mode"] = (int)s.seasonMode;
    JsonArray arr = doc.createNestedArray("profiles");
    for (const SeasonProfile& p : s.profiles) {
        JsonObject o = arr.createNestedObject();
        o["heavy_on_kwh"] = r2(p.heavyOnKwh);
        o["heavy_off_kwh"] = r2(p.heavyOffKwh);
        o["gen_auto"] = p.genAuto;
        o["gen_start_kwh"] = r2(p.genStartKwh);
        o["gen_stop_kwh"] = r2(p.genStopKwh);
    }
    sendJson(doc);
}

static void handlePostSettings() {
    if (!authed()) return;
    DynamicJsonDocument doc(1024);
    if (deserializeJson(doc, s_server->arg("plain"))) {
        s_server->send(400, "text/plain", "bad JSON");
        return;
    }
    Settings s = s_relays->currentSettings();
    s.capacityKwh = doc["capacity_kwh"] | s.capacityKwh;
    s.seasonMode = (SeasonMode)(doc["season_mode"] | (int)s.seasonMode);
    JsonArray arr = doc["profiles"];
    if (arr.size() == kSeasonCount) {
        for (int i = 0; i < kSeasonCount; i++) {
            JsonObject o = arr[i];
            SeasonProfile& p = s.profiles[i];
            p.heavyOnKwh  = o["heavy_on_kwh"]  | p.heavyOnKwh;
            p.heavyOffKwh = o["heavy_off_kwh"] | p.heavyOffKwh;
            p.genAuto     = o["gen_auto"]      | p.genAuto;
            p.genStartKwh = o["gen_start_kwh"] | p.genStartKwh;
            p.genStopKwh  = o["gen_stop_kwh"]  | p.genStopKwh;
        }
    }
    String err = settings::validate(s);
    if (!err.isEmpty()) {
        s_server->send(400, "text/plain", err);
        return;
    }
    settings::save(s);
    s_relays->applySettings(s);
    Log.println("[Web] settings updated");
    s_server->send(200, "text/plain", "ok");
}

// Body as text/plain (what the UI's fetch() sends) or ?mode=... — the
// server drops form-encoded fields without '=', so a bare form word is lost.
static String bodyWord(const char* argName) {
    String v = s_server->arg("plain");
    if (v.isEmpty()) v = s_server->arg(argName);
    return v;
}

static void handleGenMode() {
    if (!authed()) return;
    GenMode m;
    String v = bodyWord("mode");
    v.trim();
    if (!RelayController::parseGenMode(v, m)) {
        s_server->send(400, "text/plain", "mode must be auto|run|off");
        return;
    }
    s_relays->setGenMode(m, "web");
    s_server->send(200, "text/plain", "ok");
}

static void handleAlertAck() {
    if (!authed()) return;
    String key = bodyWord("key");
    key.trim();
    if (!alerts.ack(key, "web")) {
        s_server->send(404, "text/plain", "no such active alarm");
        return;
    }
    s_server->send(200, "text/plain", "ok");
}

static void handleNotify() {
    if (!authed()) return;
    DynamicJsonDocument doc(8192);
    alerts.fillTelegram(doc.createNestedObject("telegram"));
    JsonObject ha = doc.createNestedObject("ha");
    alerts.fillHa(ha);
    JsonObject sys = doc.createNestedObject("system");
    s_fillSystem(sys);
    ha["mqtt"] = sys["mqtt_ha"];
    ha["tls"] = sys["ha_tls"];
    doc.remove("system");
    sendJson(doc);
}

static void handleTelegramToken() {
    if (!authed()) return;
#if defined(ESP8266)
    s_server->send(400, "text/plain", "На NodeMCU Telegram напряму неможливий (мало пам'яті для TLS). Тривоги йдуть через HA.");
    return;
#endif
    String t = s_server->arg("plain");
    t.trim();
    // Bot tokens look like 123456789:AA... — reject obvious paste mistakes.
    if (!t.isEmpty() && (t.indexOf(':') < 5 || t.length() < 30 || t.indexOf(' ') >= 0)) {
        s_server->send(400, "text/plain", "це не схоже на токен бота (має бути 123456789:AA...)");
        return;
    }
    alerts.setTelegramToken(t);
    s_server->send(200, "text/plain", "ok");
}

static void handleTelegramChat() {
    if (!authed()) return;
    String c = s_server->arg("plain");
    c.trim();
    if (c.isEmpty() || c.toInt() == 0) {
        s_server->send(400, "text/plain", "bad chat id");
        return;
    }
    alerts.setTelegramChat(c);
    s_server->send(200, "text/plain", "ok");
}

static void handleTelegramTest() {
    if (!authed()) return;
    alerts.sendTest();
    s_server->send(200, "text/plain", "ok");
}

static void handleHeavyMode() {
    if (!authed()) return;
    HeavyMode m;
    String v = bodyWord("mode");
    v.trim();
    if (!RelayController::parseHeavyMode(v, m)) {
        s_server->send(400, "text/plain", "mode must be auto|on|off");
        return;
    }
    s_relays->setHeavyMode(m, "web");
    s_server->send(200, "text/plain", "ok");
}

static void handleGenFeedback() {
    if (!authed()) return;
    String v = bodyWord("enabled");
    v.trim();
    if (v != "on" && v != "off") { s_server->send(400, "text/plain", "on|off"); return; }
    s_relays->setGenFeedbackEnabled(v == "on");
    s_server->send(200, "text/plain", "ok");
}

static void handleResetState() {
    if (!authed()) return;
    s_relays->resetState();
    s_server->send(200, "text/plain", "ok");
}

static void handleReboot() {
    if (!authed()) return;
    Log.println("[Web] reboot requested");
    s_server->send(200, "text/plain", "rebooting");
    delay(300);
    ESP.restart();
}

static void handleUpdateDone() {
    if (!authed()) return;
    if (Update.hasError() || !s_updateError.isEmpty()) {
        s_server->send(500, "text/plain", s_updateError.isEmpty() ? "update failed" : s_updateError);
        return;
    }
    s_server->send(200, "text/plain", "ok");
    Log.println("[OTA] web update done, rebooting");
    delay(500);
    ESP.restart();
}

static String updateError() {
#if defined(ESP8266)
    return Update.getErrorString();
#else
    return Update.errorString();
#endif
}

static void handleUpdateUpload() {
    HTTPUpload& up = s_server->upload();
    compatFeedWatchdog();   // the whole upload runs inside one handleClient()
    if (up.status == UPLOAD_FILE_START) {
        s_updateError = "";
        if (!s_server->authenticate(WEB_USERNAME, WEB_PASSWORD)) {
            s_updateError = "unauthorized";
            return;
        }
        Log.printf("[OTA] web update start: %s\n", up.filename.c_str());
#if defined(ESP8266)
        uint32_t space = (ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000;
        if (!Update.begin(space)) s_updateError = Update.getErrorString();
#else
        if (!Update.begin(UPDATE_SIZE_UNKNOWN)) s_updateError = Update.errorString();
#endif
    } else if (up.status == UPLOAD_FILE_WRITE) {
        if (s_updateError.isEmpty() && Update.write(up.buf, up.currentSize) != up.currentSize)
            s_updateError = updateError();
    } else if (up.status == UPLOAD_FILE_END) {
        if (s_updateError.isEmpty() && !Update.end(true)) s_updateError = updateError();
        if (s_updateError.isEmpty()) Log.printf("[OTA] web update: %u bytes written\n", up.totalSize);
        else Log.printf("[OTA] web update failed: %s\n", s_updateError.c_str());
    } else if (up.status == UPLOAD_FILE_ABORTED) {
#if defined(ESP8266)
        Update.end();
#else
        Update.abort();
#endif
        s_updateError = "upload aborted";
    }
}

namespace web_ui {

void begin(CompatWebServer& server, RelayController& relays, SystemFiller fillSystem) {
    s_server = &server;
    s_relays = &relays;
    s_fillSystem = fillSystem;

#if defined(ESP8266)
    server.collectHeaders("Accept");
#else
    static const char* headers[] = {"Accept"};
    server.collectHeaders(headers, 1);
#endif

    server.on("/", HTTP_GET, handleRoot);
    server.on("/api/status", HTTP_GET, handleStatus);
    server.on("/api/log", HTTP_GET, handleLog);
    server.on("/api/settings", HTTP_GET, handleGetSettings);
    server.on("/api/settings", HTTP_POST, handlePostSettings);
    server.on("/api/gen_mode", HTTP_POST, handleGenMode);
    server.on("/api/heavy_mode", HTTP_POST, handleHeavyMode);
    server.on("/api/gen_feedback", HTTP_POST, handleGenFeedback);
    server.on("/api/reset_state", HTTP_POST, handleResetState);
    server.on("/api/alerts/ack", HTTP_POST, handleAlertAck);
    server.on("/api/notify", HTTP_GET, handleNotify);
    server.on("/api/telegram/token", HTTP_POST, handleTelegramToken);
    server.on("/api/telegram/chat", HTTP_POST, handleTelegramChat);
    server.on("/api/telegram/test", HTTP_POST, handleTelegramTest);
    server.on("/api/reboot", HTTP_POST, handleReboot);
    server.on("/update", HTTP_POST, handleUpdateDone, handleUpdateUpload);
}

}  // namespace web_ui
