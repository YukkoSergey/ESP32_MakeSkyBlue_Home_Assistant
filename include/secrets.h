#ifndef SECRETS_H
#define SECRETS_H

// WiFi
#define WIFI_SSID "YukkoS"
#define WIFI_PASSWORD "papaj0nes"

// MQTT
#define MQTT_HOST "192.168.31.251"
#define MQTT_PORT 1883
#define MQTT_USERNAME "user"
#define MQTT_PASSWORD "pass"

// Tuya Cloud
#define TUYA_CLIENT_ID "yr5exmwu3mfqnpspwecj"
#define TUYA_CLIENT_SECRET "e30f9fb1cf0a4eada2d9d224be8879c1"
#define TUYA_API_BASE_URL "https://openapi.tuyaeu.com" // Restored correct URL

// Device IDs

// Device IDs
#define TUYA_MUST_DEVICE_ID "vdevo176935261381318"
#define TUYA_VIRTUAL_ID TUYA_MUST_DEVICE_ID
#define TUYA_MPPT_1_ID "bf9f66c523a9f51ccdlcbl"
#define TUYA_MPPT_2_ID "bfc760575f2be3faddvbnb"
#define TUYA_MPPT_3_ID "bf065afbc7e8f1d2381gtq"

// Solar Manager Serials
#define SM_MPPT_1_SERIAL "SN1"
#define SM_MPPT_2_SERIAL "SN2"
#define SM_MPPT_3_SERIAL "SN3"

#endif