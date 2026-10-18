#include "alerts.h"
#include "app_log.h"
#include "secrets.h"
#include <WiFiClientSecure.h>
#if defined(ESP8266)
#include <ESP8266HTTPClient.h>
#else
#include <HTTPClient.h>
#endif
#include <time.h>

Alerts alerts;

// api.telegram.org chains to Go Daddy Root G2 (valid until 2037-12-31).
// SHA-256 45:14:0B:32:47:EB:9C:C8:C5:B4:F0:D7:B5:30:91:F7:32:92:08:9E:6E:5A:63:E2:74:9D:D3:AC:A9:19:8E:DA
static const char kTelegramRootCa[] PROGMEM = R"PEM(-----BEGIN CERTIFICATE-----
MIIDxTCCAq2gAwIBAgIBADANBgkqhkiG9w0BAQsFADCBgzELMAkGA1UEBhMCVVMx
EDAOBgNVBAgTB0FyaXpvbmExEzARBgNVBAcTClNjb3R0c2RhbGUxGjAYBgNVBAoT
EUdvRGFkZHkuY29tLCBJbmMuMTEwLwYDVQQDEyhHbyBEYWRkeSBSb290IENlcnRp
ZmljYXRlIEF1dGhvcml0eSAtIEcyMB4XDTA5MDkwMTAwMDAwMFoXDTM3MTIzMTIz
NTk1OVowgYMxCzAJBgNVBAYTAlVTMRAwDgYDVQQIEwdBcml6b25hMRMwEQYDVQQH
EwpTY290dHNkYWxlMRowGAYDVQQKExFHb0RhZGR5LmNvbSwgSW5jLjExMC8GA1UE
AxMoR28gRGFkZHkgUm9vdCBDZXJ0aWZpY2F0ZSBBdXRob3JpdHkgLSBHMjCCASIw
DQYJKoZIhvcNAQEBBQADggEPADCCAQoCggEBAL9xYgjx+lk09xvJGKP3gElY6SKD
E6bFIEMBO4Tx5oVJnyfq9oQbTqC023CYxzIBsQU+B07u9PpPL1kwIuerGVZr4oAH
/PMWdYA5UXvl+TW2dE6pjYIT5LY/qQOD+qK+ihVqf94Lw7YZFAXK6sOoBJQ7Rnwy
DfMAZiLIjWltNowRGLfTshxgtDj6AozO091GB94KPutdfMh8+7ArU6SSYmlRJQVh
GkSBjCypQ5Yj36w6gZoOKcUcqeldHraenjAKOc7xiID7S13MMuyFYkMlNAJWJwGR
tDtwKj9useiciAF9n9T521NtYJ2/LOdYq7hfRvzOxBsDPAnrSTFcaUaz4EcCAwEA
AaNCMEAwDwYDVR0TAQH/BAUwAwEB/zAOBgNVHQ8BAf8EBAMCAQYwHQYDVR0OBBYE
FDqahQcQZyi27/a9BUFuIMGU2g/eMA0GCSqGSIb3DQEBCwUAA4IBAQCZ21151fmX
WWcDYfF+OwYxdS2hII5PZYe096acvNjpL9DbWu7PdIxztDhC2gV7+AJ1uP2lsdeu
9tfeE8tTEH6KRtGX+rcuKxGrkLAngPnon1rpN5+r5N9ss4UXnT3ZJE95kTXWXwTr
gIOrmgIttRD02JDHBHNA7XIloKmf7J6raBKZV8aPEjoJpL1E/QYVN8Gb5DKj7Tjo
2GTzLH4U/ALqn83/B2gX2yKQOC16jdFU8WnjXzPKej17CuPKf1855eJ1usV2GDPO
LPAvTK33sefOT6jEm0pUBsV/fdUID+Ic/n4XuKxe9tQWskMJDE32p2u0mYRlynqI
4uJEvlz36hz1
-----END CERTIFICATE-----
)PEM";

static const char* kKeyNames[Alerts::kCount] = {"stale", "frozen", "voltage", "ha_down", "broker_down", "gen_no_start"};

// --- shared between the main loop, the web UI and the Telegram task ----------
struct OutMsg   { String text; int ackKey; };   // ackKey < 0: no button
struct SeenChat { String id, name, text; time_t when; };
struct SentMsg  { time_t when; String text; bool ok; };

static CompatMutex s_mutex;
static std::vector<OutMsg>   s_queue;
static std::vector<SeenChat> s_seen;      // chats that wrote to the bot but aren't ours
static std::vector<SentMsg>  s_recent;    // last few sends, newest last
static String   s_statusText;             // for /status, rebuilt by tick()
static String   s_token, s_chat, s_botName, s_lastError;
static bool     s_tokenChanged = false;
static volatile bool s_pendingAck[Alerts::kCount];
static volatile uint32_t s_lastOkMs = 0;
static constexpr size_t kMaxQueue = 30, kMaxSeen = 5, kMaxRecent = 10;

static void lock()   { s_mutex.lock(); }
static void unlock() { s_mutex.unlock(); }
static void setError(const String& e) { lock(); s_lastError = e; unlock(); }

static void enqueueTelegram(const String& text, int ackKey = -1) {
    lock();
    if (s_queue.size() >= kMaxQueue) s_queue.erase(s_queue.begin());   // drop oldest
    s_queue.push_back({text, ackKey});
    unlock();
}

// --- Telegram task ------------------------------------------------------------
#if defined(ESP8266)
// BearSSL: parse the root once (into RAM — PROGMEM can't be read bytewise),
// and use small TLS buffers if the server agrees (saves ~16 KB of ~45 KB).
static BearSSL::X509List* s_trust = nullptr;
static int s_mfln = -1;   // -1 unknown, 0 no, 1 yes
#endif

static bool tgCall(const String& token, const char* method, const String& body, String* response) {
#if defined(ESP8266)
    if (!s_trust) s_trust = new BearSSL::X509List(String(FPSTR(kTelegramRootCa)).c_str());
    BearSSL::WiFiClientSecure tls;
    tls.setTrustAnchors(s_trust);
    if (s_mfln < 0) s_mfln = tls.probeMaxFragmentLength("api.telegram.org", 443, 1024) ? 1 : 0;
    if (s_mfln == 1) tls.setBufferSizes(1024, 1024);
#else
    WiFiClientSecure tls;
    tls.setCACert(kTelegramRootCa);
#endif
    HTTPClient http;
    http.setTimeout(10000);
    String url = "https://api.telegram.org/bot" + token + "/" + method;
    if (!http.begin(tls, url)) { setError("begin failed"); return false; }
    http.addHeader("Content-Type", "application/json");
    int code = http.POST(body);
    if (response) *response = http.getString();
    http.end();
    if (code != 200) {
        setError(String(method) + ": HTTP " + code + (code == 401 ? " (невірний токен)" : code < 0 ? " (немає зв'язку)" : ""));
        return false;
    }
    return true;
}

static bool tgSend(const String& token, const String& chat, const OutMsg& m) {
    DynamicJsonDocument doc(1024 + m.text.length());
    doc["chat_id"] = chat;
    doc["text"] = m.text;
    if (m.ackKey >= 0) {
        JsonObject btn = doc.createNestedObject("reply_markup")
                            .createNestedArray("inline_keyboard")
                            .createNestedArray()
                            .createNestedObject();
        btn["text"] = "✅ Прийняв";
        btn["callback_data"] = String("ack:") + kKeyNames[m.ackKey];
    }
    String body;
    serializeJson(doc, body);
    return tgCall(token, "sendMessage", body, nullptr);
}

static void tgPoll(const String& token, const String& chat, int64_t& offset) {
#if defined(ESP8266)
    const int limit = 2; const size_t docSize = 3072;
#else
    const int limit = 5; const size_t docSize = 6144;
#endif
    String body = "{\"timeout\":0,\"limit\":" + String(limit) + ",\"offset\":" + String((long long)offset) + "}";
    String resp;
    if (!tgCall(token, "getUpdates", body, &resp)) return;
    DynamicJsonDocument doc(docSize);
    if (deserializeJson(doc, resp)) return;
    for (JsonObject u : doc["result"].as<JsonArray>()) {
        offset = (int64_t)u["update_id"].as<long long>() + 1;
        if (u.containsKey("callback_query")) {
            JsonObject cq = u["callback_query"];
            String from = cq["message"]["chat"]["id"].as<String>();
            String data = cq["data"].as<String>();
            if (from == chat && data.startsWith("ack:")) {
                for (int i = 0; i < Alerts::kCount; i++)
                    if (data.substring(4) == kKeyNames[i]) s_pendingAck[i] = true;
            }
            tgCall(token, "answerCallbackQuery",
                   "{\"callback_query_id\":\"" + cq["id"].as<String>() + "\",\"text\":\"Прийнято\"}", nullptr);
        } else if (u.containsKey("message")) {
            JsonObject msg = u["message"];
            String from = msg["chat"]["id"].as<String>();
            String text = msg["text"].as<String>();
            if (from == chat) {
                if (text.startsWith("/status")) {
                    lock();
                    String st = s_statusText;
                    unlock();
                    enqueueTelegram(st);
                }
                continue;
            }
            // Not our chat: remember it so the web UI can offer it.
            String name = msg["chat"]["title"] | "";
            if (name.isEmpty()) name = String(msg["chat"]["first_name"] | "") + " " + String(msg["chat"]["last_name"] | "");
            String user = msg["chat"]["username"] | "";
            if (!user.isEmpty()) name += " (@" + user + ")";
            lock();
            for (size_t i = 0; i < s_seen.size(); i++)
                if (s_seen[i].id == from) { s_seen.erase(s_seen.begin() + i); break; }
            if (s_seen.size() >= kMaxSeen) s_seen.erase(s_seen.begin());
            s_seen.push_back({from, name, text, time(nullptr)});
            unlock();
            Log.printf("[Telegram] message from chat %s (%s) — choose it on the web page\n",
                       from.c_str(), name.c_str());
        }
    }
}

// ESP8266: no direct Telegram. api.telegram.org doesn't do TLS Max Fragment
// Length, so BearSSL needs a ~17 KB receive buffer in one piece and the
// NodeMCU has ~14-16 KB free — the first attempt crashed the board in a
// reboot loop. Alerts still go to HA (dacha_relays/alert); a small service
// next to HA forwards them to Telegram.
#if defined(ESP8266)
static const char* kTelegramUnsupported =
    "NodeMCU не має пам'яті для TLS-з'єднання з Telegram (потрібно ~17 КБ суцільної пам'яті, "
    "є ~15 КБ, а малі буфери сервер Telegram не підтримує). Тривоги йдуть через HA; у Telegram "
    "їх пересилатиме сервіс на VPS.";
#endif

// One step of the Telegram worker: at most one HTTPS call. Driven by a task
// on ESP32 (never on ESP8266, see above).
static void telegramStep() {
    static int64_t offset = 0;
    static uint32_t lastPoll = 0, retryAt = 0;
    {
        if (WiFi.status() != WL_CONNECTED || time(nullptr) < 1700000000) return;   // TLS needs time
        lock();
        String token = s_token, chat = s_chat;
        bool changed = s_tokenChanged;
        s_tokenChanged = false;
        unlock();
        if (token.isEmpty()) return;

        if (changed) {
            offset = 0;
            String resp;
            String name;
            if (tgCall(token, "getMe", "{}", &resp)) {
                DynamicJsonDocument d(1024);
                if (!deserializeJson(d, resp)) name = d["result"]["username"].as<String>();
                setError("");
                Log.printf("[Telegram] bot @%s\n", name.c_str());
            }
            lock();
            s_botName = name;
            unlock();
        }

        if (!chat.isEmpty() && millis() >= retryAt) {
            lock();
            bool have = !s_queue.empty();
            OutMsg m = have ? s_queue.front() : OutMsg{};
            unlock();
            if (have) {
                bool ok = tgSend(token, chat, m);
                lock();
                if (ok) {
                    s_lastOkMs = millis();
                    s_lastError = "";
                    if (!s_queue.empty()) s_queue.erase(s_queue.begin());
                }
                if (s_recent.size() >= kMaxRecent) s_recent.erase(s_recent.begin());
                s_recent.push_back({time(nullptr), m.text, ok});
                String err = s_lastError;
                unlock();
                if (!ok) {
                    Log.printf("[Telegram] send failed (%s), retry in 30 s\n", err.c_str());
                    retryAt = millis() + 30000;
                }
                return;
            }
        }
        // Poll faster while waiting for the first /start.
        if (millis() - lastPoll > (chat.isEmpty() ? 5000u : 15000u)) {
            lastPoll = millis();
            tgPoll(token, chat, offset);
        }
    }
}

#if defined(ESP32)
static void telegramTask(void*) {
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        telegramStep();
    }
}
#endif

void Alerts::service() {
    // ESP8266: nothing to do — direct Telegram is disabled there (see above).
}

// --- Alerts (main loop side) --------------------------------------------------
const char* Alerts::title(Key k) {
    switch (k) {
        case Stale:         return "немає даних від Solar Assistant";
        case Frozen:        return "SOC батареї застиг";
        case VoltEmergency: return "аварійно низька напруга батареї";
        case HaDown:        return "Home Assistant не відповідає";
        case BrokerDown:    return "MQTT-брокер Home Assistant недоступний";
        case GenNoStart:    return "генератор не працює, хоча плата його запустила";
        default:            return "?";
    }
}

static const char* resetReasonUa(ResetKind r) {
    switch (r) {
        case ResetKind::PowerOn:  return "увімкнення живлення";
        case ResetKind::Software: return "програмний перезапуск (оновлення / кнопка)";
        case ResetKind::Crash:    return "ЗБІЙ ПРОШИВКИ (panic)";
        case ResetKind::Watchdog: return "ЗАВИСАННЯ, спрацював watchdog";
        case ResetKind::Brownout: return "ПРОСІДАННЯ ЖИВЛЕННЯ плати (brownout)";
        default:                  return "інша причина";
    }
}

void Alerts::begin(ResetKind resetKind) {
    s_mutex.init();
    _resetKind = resetKind;

    Preferences p;
    p.begin("tg", true);
    s_token = p.getString("token", "");
    s_chat  = p.getString("chat", "");
    p.end();
#ifdef TELEGRAM_BOT_TOKEN
    if (s_token.isEmpty()) s_token = TELEGRAM_BOT_TOKEN;
#endif
#ifdef TELEGRAM_CHAT_ID
    if (s_chat.isEmpty()) s_chat = TELEGRAM_CHAT_ID;
#endif
#if defined(ESP8266)
    // Never keep a token here: one is what crash-looped the board.
    if (!s_token.isEmpty()) {
        Preferences w;
        w.begin("tg", false);
        w.putString("token", "");
        w.putString("chat", "");
        w.end();
        s_token = "";
        s_chat = "";
    }
    s_lastError = kTelegramUnsupported;
#endif
    s_tokenChanged = !s_token.isEmpty();
    Log.printf("[Telegram] %s\n", s_token.isEmpty() ? "not configured — set it up on the web page"
                                  : s_chat.isEmpty() ? "token set, waiting for /start to pick the chat"
                                                     : "configured");
#if defined(ESP32)
    xTaskCreatePinnedToCore(telegramTask, "telegram", 12288, nullptr, 1, nullptr, 1);
#endif
}

void Alerts::setTelegramToken(const String& token) {
#if defined(ESP8266)
    (void)token;
    return;   // see kTelegramUnsupported
#endif
    Preferences p;
    p.begin("tg", false);
    p.putString("token", token);
    if (token.isEmpty()) p.putString("chat", "");
    p.end();
    lock();
    s_token = token;
    s_tokenChanged = true;
    s_botName = "";
    s_lastError = "";
    s_seen.clear();
    if (token.isEmpty()) s_chat = "";
    unlock();
    Log.printf("[Telegram] token %s from the web page\n", token.isEmpty() ? "removed" : "set");
}

void Alerts::setTelegramChat(const String& chatId) {
    Preferences p;
    p.begin("tg", false);
    p.putString("chat", chatId);
    p.end();
    lock();
    s_chat = chatId;
    for (size_t i = 0; i < s_seen.size(); i++)
        if (s_seen[i].id == chatId) { s_seen.erase(s_seen.begin() + i); break; }
    unlock();
    Log.printf("[Telegram] chat set to %s from the web page\n", chatId.c_str());
    enqueueTelegram("✅ Дача: цей чат тепер отримує тривоги плати. Команда /status показує поточний стан.");
}

void Alerts::sendTest() {
    enqueueTelegram("🧪 Дача: тестове повідомлення з веб-сторінки плати.");
}

void Alerts::send(const char* key, const char* level, const String& ttl, const String& text, int ackKey) {
    static const char* icon[] = {"🚨", "🔁", "✅", "ℹ️", "⚠️"};
    String lv(level);
    const char* ic = lv == "alarm" ? icon[0] : lv == "repeat" ? icon[1] : lv == "resolved" ? icon[2]
                   : lv == "warning" ? icon[4] : icon[3];
    String tg = String(ic) + " Дача: " + ttl + (text.isEmpty() ? "" : ".\n" + text);
    Log.printf("[Alert] %s\n", tg.c_str());
    enqueueTelegram(tg, ackKey);

    bool toHa = ackKey < 0 || viaHa((Key)ackKey);
    if (String(key) == "ha_down" || String(key) == "broker_down") toHa = false;
    if (!toHa) return;
    DynamicJsonDocument d(1024);   // heap: the ESP8266 stack is only 4 KB
    d["key"] = key;
    d["level"] = level;
    d["title"] = String(ic) + " Дача: " + ttl;
    d["text"] = text;
    d["ts"] = (long)time(nullptr);
    String json;
    serializeJson(d, json);
    if (_haOut.size() >= kMaxQueue) _haOut.erase(_haOut.begin());
    _haOut.push_back(json);
}

bool Alerts::peekHaMessage(String& json) const {
    if (_haOut.empty()) return false;
    json = _haOut.front();
    return true;
}

void Alerts::dropHaMessage() {
    if (!_haOut.empty()) _haOut.erase(_haOut.begin());
}

void Alerts::raise(Key k, const String& detail) {
    Alarm& a = _alarms[k];
    a.detail = detail;
    if (a.active) return;
    a.active = true;
    a.acked = false;
    a.sinceMs = millis();
    a.lastSentMs = millis();
    send(a.key, "alarm", title(k), detail, k);
}

void Alerts::clear(Key k) {
    Alarm& a = _alarms[k];
    if (!a.active) return;
    a.active = false;
    send(a.key, "resolved", String("минуло — ") + title(k),
         "Тривало " + String((millis() - a.sinceMs) / 60000) + " хв.", -1);
}

bool Alerts::ack(const String& key, const char* by) {
    for (int i = 0; i < kCount; i++) {
        if (key == _alarms[i].key && _alarms[i].active && !_alarms[i].acked) {
            _alarms[i].acked = true;
            Log.printf("[Alert] '%s' acknowledged (%s)\n", title((Key)i), by);
            return true;
        }
    }
    return false;
}

void Alerts::onHaHeartbeat() {
    if (!_lastHeartbeatMs) Log.println("[HA] heartbeat received — HA watchdog active");
    _lastHeartbeatMs = millis();
    _haOffline = false;
}

void Alerts::onHaStatus(bool online) {
    if (_haOffline == !online) return;
    _haOffline = !online;
    Log.printf("[HA] homeassistant/status: %s\n", online ? "online" : "offline");
    if (online) _lastHeartbeatMs = _lastHeartbeatMs ? millis() : 0;
}

void Alerts::tick(const RelayController& r, bool haMqttConnected) {
    uint32_t now = millis();

    for (int i = 0; i < kCount; i++) {
        if (s_pendingAck[i]) { s_pendingAck[i] = false; ack(_alarms[i].key, "Telegram"); }
    }

    if (!_bootReported && (WiFi.status() == WL_CONNECTED)) {
        _bootReported = true;
        bool crash = _resetKind == ResetKind::Crash || _resetKind == ResetKind::Watchdog ||
                     _resetKind == ResetKind::Brownout;
        send("boot", crash ? "warning" : "info",
             String("плата запустилась, причина: ") + resetReasonUa(_resetKind), "", -1);
    }

    // Stale SA data.
    if (!r.socFresh() && now > kBootGraceMs) {
        raise(Stale, "Важке навантаження вимкнено (економ-режим). Перевір Solar Assistant, WiFi у сараї "
                     "і MQTT-брокер 192.168.31.251.");
    } else if (r.socFresh()) {
        clear(Stale);
    }

    // Frozen SOC.
    if (r.socFrozen()) {
        raise(Frozen, String("Значення стоїть на ") + r.soc() + " %: " + r.frozenReason() +
                      ".\nСхоже, Solar Assistant не читає Pylontech (USB / консоль). Важке навантаження "
                      "вимкнено, генератор лишається як був. Перезапусти SA або перевір кабель до батарей.");
    } else {
        clear(Frozen);
    }

    // Voltage emergency.
    if (r.voltEmergency()) {
        raise(VoltEmergency, String("Батарея нижче ") + String(RelayController::kVoltEmergencyV, 1) +
                             " В понад 5 хв — вона майже порожня, хоч би що показував SOC. Важке навантаження "
                             "вимкнено, генератор запускається (якщо не заборонений вручну). Перевір паливо.");
    } else {
        clear(VoltEmergency);
    }

    // Generator commanded but not running (feedback contact).
    if (r.genNoStartAlarm()) {
        raise(GenNoStart, "Реле генератора замкнуте вже понад 3 хв, а контакт зворотного зв'язку не замкнувся "
                          "(або розімкнувся). Генератор не завівся або зупинився сам: перевір паливо, "
                          "акумулятор генератора і АВР.");
    } else {
        clear(GenNoStart);
    }

    // Real generator start / stop from the feedback contact.
    if (r.genFeedbackEnabled() && r.genFeedbackRunning() != _lastFbRunning) {
        _lastFbRunning = r.genFeedbackRunning();
        send("generator_fb", "info", String("генератор ") + (_lastFbRunning ? "реально ЗАПРАЦЮВАВ" : "реально зупинився"),
             _lastFbRunning && !r.generatorOn() ? "Плата його не запускала: завели вручну або власна автоматика генератора." : "", -1);
    }

    // HA broker reachability.
    if (haMqttConnected || WiFi.status() != WL_CONNECTED) {
        _haDisconnectedSinceMs = 0;
        if (haMqttConnected) clear(BrokerDown);
    } else {
        if (!_haDisconnectedSinceMs) _haDisconnectedSinceMs = now;
        if (now - _haDisconnectedSinceMs >= kBrokerDownMs)
            raise(BrokerDown, String("Плата не може підключитись до ") + MQTT_HA_HOST + ":" + MQTT_HA_PORT +
                              " вже " + String((now - _haDisconnectedSinceMs) / 60000) +
                              " хв. HA не отримує даних і не надішле push. Перевір VPS.");
    }

    // HA itself (only once its heartbeat automation has been seen).
    if (haMqttConnected && _haOffline) {
        raise(HaDown, "HA повідомив через MQTT, що він offline (LWT). Push-сповіщень з телефона не буде.");
    } else if (haMqttConnected && _lastHeartbeatMs && now - _lastHeartbeatMs >= kHaHeartbeatMs) {
        raise(HaDown, "Немає пульсу від HA вже " + String((now - _lastHeartbeatMs) / 60000) +
                      " хв, хоча брокер працює. Push-сповіщень з телефона не буде. Перевір HA на VPS.");
    } else if (haMqttConnected && !_haOffline && _lastHeartbeatMs && now - _lastHeartbeatMs < kHaHeartbeatMs) {
        clear(HaDown);
    }

    // Repeat unacknowledged alarms. millis() rather than `now`: raise() above
    // may have stamped lastSentMs after `now` was taken, and the unsigned
    // difference would wrap into an instant repeat.
    for (int i = 0; i < kCount; i++) {
        Alarm& a = _alarms[i];
        uint32_t t = millis();
        if (a.active && !a.acked && t - a.lastSentMs >= kRepeatMs) {
            a.lastSentMs = t;
            send(a.key, "repeat", String("нагадування (не прийнято): ") + title((Key)i), a.detail, i);
        }
    }

    // Generator start / stop.
    if (!_genKnown) { _genKnown = true; _lastGen = r.generatorOn(); }
    if (r.generatorOn() != _lastGen) {
        _lastGen = r.generatorOn();
        String res = r.socTrusted() ? String(r.reserveKwh(), 1) + " кВт·год" : String("невідомий");
        send("generator", "info", String("генератор ") + (_lastGen ? "ЗАПУЩЕНО" : "зупинено"),
             String("Режим ") + RelayController::genModeName(r.genMode()) + ", запас " + res + ".", -1);
    }

    // Text for /status.
    static uint32_t lastStatus = 0;
    if (now - lastStatus > 5000) {
        lastStatus = now;
        String st = "📊 Дача: SOC " + String(r.soc()) + " %, запас " + String(r.reserveKwh(), 1) + " кВт·год, сезон " +
                    settings::seasonName(r.activeSeason()) + ".\nВажке навантаження: " +
                    (r.heavyLoadOn() ? "дозволене" : "економ") + ". Генератор: " +
                    (r.generatorOn() ? "працює" : "стоїть") + " (режим " + RelayController::genModeName(r.genMode()) + ").";
        for (int i = 0; i < kCount; i++)
            if (_alarms[i].active) st += String("\n🚨 ") + title((Key)i) + (_alarms[i].acked ? " (прийнято)" : "");
        lock();
        s_statusText = st;
        unlock();
    }
}

void Alerts::fillStatus(JsonObject o) const {
    JsonArray arr = o.createNestedArray("active");
    for (int i = 0; i < kCount; i++) {
        const Alarm& a = _alarms[i];
        if (!a.active) continue;
        JsonObject x = arr.createNestedObject();
        x["key"] = a.key;
        x["title"] = title((Key)i);
        x["detail"] = a.detail;
        x["acked"] = a.acked;
        x["since_min"] = (millis() - a.sinceMs) / 60000;
    }
    lock();
    o["telegram"] = !s_token.isEmpty() && !s_chat.isEmpty();
    o["telegram_token"] = !s_token.isEmpty();
    o["queue"] = s_queue.size();
    o["last_error"] = s_lastError;
    unlock();
    o["last_ok_s"] = s_lastOkMs ? (long)((millis() - s_lastOkMs) / 1000) : -1L;
}

void Alerts::fillTelegram(JsonObject o) const {
#if defined(ESP8266)
    o["unsupported"] = kTelegramUnsupported;
#endif
    lock();
    o["token_set"] = !s_token.isEmpty();
    o["token_hint"] = s_token.length() > 6 ? "…" + s_token.substring(s_token.length() - 4) : String("");
    o["bot"] = s_botName;
    o["chat"] = s_chat;
    o["queue"] = s_queue.size();
    o["last_error"] = s_lastError;
    JsonArray seen = o.createNestedArray("seen");
    for (const SeenChat& c : s_seen) {
        JsonObject x = seen.createNestedObject();
        x["id"] = c.id; x["name"] = c.name; x["text"] = c.text; x["when"] = (long)c.when;
    }
    JsonArray rec = o.createNestedArray("recent");
    for (int i = (int)s_recent.size() - 1; i >= 0; i--) {
        JsonObject x = rec.createNestedObject();
        x["when"] = (long)s_recent[i].when; x["text"] = s_recent[i].text; x["ok"] = s_recent[i].ok;
    }
    unlock();
    o["last_ok_s"] = s_lastOkMs ? (long)((millis() - s_lastOkMs) / 1000) : -1L;
}

void Alerts::fillHa(JsonObject o) const {
    o["heartbeat_seen"] = _lastHeartbeatMs != 0;
    o["heartbeat_age_s"] = _lastHeartbeatMs ? (long)((millis() - _lastHeartbeatMs) / 1000) : -1L;
    o["ha_offline"] = _haOffline;
    o["pending"] = _haOut.size();
}
