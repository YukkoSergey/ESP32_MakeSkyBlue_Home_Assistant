#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <time.h>

#include "tuya_cloud_client.h"
#include "mppt_bridge.h"
#include "solar_assistant_bridge.h"
#include "solar_manager_protocol.h"
#include "secrets.h"

const char* mqttHost = MQTT_HOST;
const int   mqttPort  = MQTT_PORT;

WiFiClient espClient;
PubSubClient mqtt(espClient);

// Використовуємо макроси з secrets.h для правильної ініціалізації підпису
TuyaCloudClient tuyaClient(TUYA_API_BASE_URL, TUYA_CLIENT_ID, TUYA_CLIENT_SECRET);
MpptBridge mpptBridge(tuyaClient);
SolarAssistantBridge saBridge(tuyaClient);

void setup_wifi() {
    Serial.print("[WiFi] Connecting to ");
    Serial.println(WIFI_SSID);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.println("\n[WiFi] Connected.");
}
void syncTime() {
    Serial.println("[Time] Syncing with NTP...");
    configTime(0, 0, "pool.ntp.org", "time.google.com", "time.nist.gov");
    time_t now;
    const time_t minEpoch = 1483228800; 
    while (true) {
        now = time(nullptr);
        if (now > minEpoch) break;
        Serial.print(".");
        delay(1000);
    }
    Serial.println("\n[Time] NTP synchronized");
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
    String topicStr = String(topic);
    topicStr.trim();
    String message = "";
    for (int i = 0; i < length; i++) message += (char)payload[i];
    message.trim();

    Serial.printf("\n[MQTT RAW] Topic: [%s] | Msg: [%s]\n", topicStr.c_str(), message.c_str());

    for (int i = 0; i < virtualMapSize; i++) {
        String mapTopic = String(virtualDeviceMap[i].mqttTopic);
        mapTopic.trim();

        if (topicStr.equalsIgnoreCase(mapTopic)) {
            Serial.printf("[V-DEV] Match found! Topic: %s -> DP: %s\n", topicStr.c_str(), virtualDeviceMap[i].tuyaCode);
        saBridge.handleMqttMessage(topicStr, message);
            return;
    }
    }

    SolarManagerProtocol::handleMqttMessage(topicStr, message, mqtt);
}
void reconnectMQTT() {
    while (!mqtt.connected()) {
        Serial.print("[MQTT] Attempting reconnect...");
        if (mqtt.connect("ESP32_Tuya_Bridge")) {
            Serial.println("connected");

            // Solar Assistant subscriptions
            mqtt.subscribe("solar_assistant/total/#");
            mqtt.subscribe("solar_assistant/inverter_1/#");

            // Solar Manager subscriptions
            // We need to subscribe to /config, /control/#, /host/# for the device serial
            // Since we don't have a dynamic serial yet, we'll use a generic pattern or hardcode for now
            // Better yet: subscribe to # or specific prefixes if known.
            // But the protocol says the device subscribes to its own /config etc.
            // For MVP, let's assume the serial is MSB_MPPT_1
            mqtt.subscribe("MSB_MPPT_1/config");
            mqtt.subscribe("MSB_MPPT_1/control/#");
            mqtt.subscribe("MSB_MPPT_1/host/#");

            SolarManagerProtocol::publishOnline(mqtt);
            } else {
            Serial.printf("failed, rc=%d. retry in 5s\n", mqtt.state());
            delay(5000);
        }
    }
}

void setup() {
    Serial.begin(115200);
    setup_wifi();
    syncTime(); 
    mqtt.setServer(mqttHost, mqttPort);
    mqtt.setCallback(mqttCallback);

    SolarManagerProtocol::init("MSB_MPPT_1", "1.0.0");

    Serial.println("[Diag] Fetching Tuya device properties...");
    String props = tuyaClient.getDeviceProperties(TUYA_MUST_DEVICE_ID);
    if (props != "") {
        Serial.println("[Diag] Tuya Properties: " + props);
    } else {
        Serial.println("[Diag] Failed to fetch properties");
    }

    Serial.println("[System] Setup complete. Entering loop...");
}

void loop() {
    if (!mqtt.connected()) {
        reconnectMQTT();
    }
    mqtt.loop();

    static unsigned long lastMpptUpdate = 0;
    if (millis() - lastMpptUpdate > 10000) { // Increased frequency for telemetry
        lastMpptUpdate = millis();
        mpptBridge.updateAll();
    }

    static unsigned long lastSaUpdate = 0;
    if (millis() - lastSaUpdate > 10000) {
        lastSaUpdate = millis();
        saBridge.processPendingCommands();
    }

    SolarManagerProtocol::processPeriodicTasks(mqtt, mpptBridge.getState(0));
}

