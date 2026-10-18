// -----------------------------------------------------------------------------
// Minimal NodeMCU bootstrap: relays released, WiFi, OTA. Flashed once over USB
// so the full relay firmware (main_nodemcu.cpp) can then go over WiFi.
//   pio run -e nodemcu_bootstrap -t upload
// -----------------------------------------------------------------------------
#if defined(ESP8266)

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ArduinoOTA.h>
#include "secrets.h"

static const char* kHostname = "dacha-relays";
static ESP8266WebServer server(80);

struct Net { const char* ssid; const char* pass; };
static const Net kNets[] = {
    { WIFI_SSID, WIFI_PASSWORD },
#ifdef WIFI_SSID_2
    { WIFI_SSID_2, WIFI_PASSWORD_2 },
#endif
};

static void connectStrongest() {
    WiFi.mode(WIFI_STA);
    WiFi.hostname(kHostname);
    WiFi.setSleepMode(WIFI_NONE_SLEEP);
    int n = WiFi.scanNetworks();
    int best = 0, bestRssi = -1000;
    for (int i = 0; i < n; i++)
        for (size_t k = 0; k < sizeof(kNets) / sizeof(kNets[0]); k++)
            if (WiFi.SSID(i) == kNets[k].ssid && WiFi.RSSI(i) > bestRssi) { bestRssi = WiFi.RSSI(i); best = k; }
    WiFi.scanDelete();
    Serial.printf("[WiFi] connecting to %s (rssi %d)\n", kNets[best].ssid, bestRssi);
    WiFi.begin(kNets[best].ssid, kNets[best].pass);
}

void setup() {
    // Active-LOW relays: HIGH = released. First thing, before anything else.
    digitalWrite(5, HIGH); pinMode(5, OUTPUT);   // D1 heavy load
    digitalWrite(4, HIGH); pinMode(4, OUTPUT);   // D2 generator
    Serial.begin(115200);
    Serial.println("\n[Boot] NodeMCU OTA bootstrap");
    connectStrongest();

    ArduinoOTA.setHostname(kHostname);
    ArduinoOTA.setPassword(OTA_PASSWORD);
    ArduinoOTA.begin();
    server.on("/", [] {
        server.send(200, "text/plain",
                    String("dacha-relays OTA bootstrap: relays released, waiting for firmware.\nIP ") +
                    WiFi.localIP().toString() + ", SSID " + WiFi.SSID() + ", RSSI " + WiFi.RSSI() + " dBm\n");
    });
    server.begin();
}

void loop() {
    static uint32_t lastTry = 0;
    static bool announced = false;
    if (WiFi.status() != WL_CONNECTED) {
        announced = false;
        if (millis() - lastTry > 20000) { lastTry = millis(); connectStrongest(); }
    } else if (!announced) {
        announced = true;
        Serial.printf("[WiFi] connected, ip=%s rssi=%d\n", WiFi.localIP().toString().c_str(), WiFi.RSSI());
    }
    ArduinoOTA.handle();
    server.handleClient();
    delay(2);
}

#endif
