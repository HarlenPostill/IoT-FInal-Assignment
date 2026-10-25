#pragma once
#include <stdint.h>

#if __has_include("secrets.h")
#include "secrets.h"
#endif
#ifndef LAB_WIFI_SSID
#define LAB_WIFI_SSID "YOUR_2.4_GHZ_WIFI"
#endif
#ifndef LAB_WIFI_PASSWORD
#define LAB_WIFI_PASSWORD "YOUR_WIFI_PASSWORD"
#endif
#ifndef LAB_GROUP_ID
#define LAB_GROUP_ID "YOUR_UNIQUE_GROUP_ID"
#endif

namespace Config {
constexpr char WIFI_SSID[] = LAB_WIFI_SSID;
constexpr char WIFI_PASSWORD[] = LAB_WIFI_PASSWORD;
constexpr char GROUP_ID[] = LAB_GROUP_ID;
// MQTT broker and credentials. InfluxDB credentials belong to Telegraf.
constexpr char MQTT_HOST[] = "broker.emqx.io";
constexpr uint16_t MQTT_PORT = 1883;
constexpr char MQTT_USER[] = "emqx";
constexpr char MQTT_PASSWORD[] = "public";
constexpr char LOCATION[] = "building_exterior";
constexpr char FIRMWARE_VERSION[] = "1.0.0";
constexpr uint8_t MIC_SCK = 26;
constexpr uint8_t MIC_WS = 25;
constexpr uint8_t MIC_SD = 33;
constexpr uint8_t LED_PIN = 2; // Same GPIO as the supplied Freenove example.
constexpr uint8_t LED_COUNT = 8;
constexpr uint8_t LED_BRIGHTNESS = 10;
constexpr uint32_t RED_FLASH_HALF_PERIOD_MS = 250; // 250 ms on, 250 ms off.
constexpr uint32_t SAMPLE_RATE = 16000;
constexpr uint32_t WINDOW_SAMPLES = SAMPLE_RATE / 2;
constexpr uint32_t PUBLISH_MS = 1000;
constexpr uint32_t STALE_MS = 2000;
constexpr float WARNING_DB = 80.0f;
constexpr float LIMIT_DB = 95.0f;
constexpr float HYSTERESIS_DB = 2.0f;
constexpr float SPL_REDUCTION_DB = 15.0f; // Subtract from the final SPL estimate.
}
