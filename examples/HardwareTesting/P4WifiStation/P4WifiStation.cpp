// Wi-Fi bring-up test for the Waveshare ESP32-P4-WIFI6.
// Copy to the local PlatformIO src/main.cpp and create src/secrets.h
// with WIFI_SSID and WIFI_PASS for a 2.4 GHz Wi-Fi network.

#include <Arduino.h>
#include <WiFi.h>
#include <esp_system.h>

#include "secrets.h"

static constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS = 30000;
static uint32_t nextStatusLog = 0;

void setup() {
    Serial.begin(115200);
    delay(1000);

    Serial.println();
    Serial.println("ESP-beatsy: Waveshare ESP32-P4-WIFI6 Wi-Fi test");
    Serial.printf("Arduino-ESP32: %d.%d.%d\n",
                  ESP_ARDUINO_VERSION_MAJOR,
                  ESP_ARDUINO_VERSION_MINOR,
                  ESP_ARDUINO_VERSION_PATCH);
    Serial.printf("ESP-IDF: %s\n", esp_get_idf_version());

    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);

    Serial.print("Connecting to Wi-Fi");
    const uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED &&
           (uint32_t)(millis() - start) < WIFI_CONNECT_TIMEOUT_MS) {
        delay(500);
        Serial.print('.');
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("Connected. IP: %s\n", WiFi.localIP().toString().c_str());
        Serial.printf("RSSI: %d dBm\n", WiFi.RSSI());
    } else {
        Serial.printf("Wi-Fi connection timed out; status=%d\n", (int)WiFi.status());
        Serial.println("Check credentials, 2.4 GHz network, and ESP-Hosted/C6 startup logs.");
    }

    nextStatusLog = millis() + 10000;
}

void loop() {
    if ((int32_t)(millis() - nextStatusLog) >= 0) {
        nextStatusLog = millis() + 10000;
        if (WiFi.status() == WL_CONNECTED) {
            Serial.printf("Wi-Fi connected: %s, RSSI %d dBm\n",
                          WiFi.localIP().toString().c_str(), WiFi.RSSI());
        } else {
            Serial.printf("Wi-Fi disconnected; status=%d\n", (int)WiFi.status());
        }
    }
    delay(10);
}
