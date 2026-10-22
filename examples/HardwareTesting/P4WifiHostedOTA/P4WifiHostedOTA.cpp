// Update the ESP32-C6 ESP-Hosted slave firmware from the P4 over Wi-Fi.
// Copy this file to the local PlatformIO project's src/main.cpp and create
// src/secrets.h with WIFI_SSID and WIFI_PASS for a 2.4 GHz Wi-Fi network.
//
// Uses ESP_HostedOTA from Arduino-ESP32 3.3.12. The upstream updater currently
// disables HTTPS certificate validation (NetworkClientSecure::setInsecure()).
// Use this only with the expected official Espressif update URL on a trusted
// network; do not repurpose it for arbitrary firmware URLs.

#include <Arduino.h>
#include <WiFi.h>
#include <ESP_HostedOTA.h>
#include <esp_system.h>

#include "secrets.h"

static constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS = 30000;

void setup() {
    Serial.begin(115200);
    delay(1000);

    Serial.println();
    Serial.println("ESP-beatsy: ESP32-P4-WIFI6 C6 Hosted firmware updater");
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

    if (WiFi.status() != WL_CONNECTED) {
        Serial.printf("Wi-Fi connection failed; status=%d\n", (int)WiFi.status());
        Serial.println("No C6 update was attempted. Check the credentials and restart.");
        return;
    }

    Serial.printf("Connected. IP: %s\n", WiFi.localIP().toString().c_str());
    Serial.printf("RSSI: %d dBm\n", WiFi.RSSI());
    Serial.println("Checking/updating the ESP-Hosted C6 firmware...");
    Serial.println("Do not power off the board while the update is in progress.");

    if (updateEspHostedSlave()) {
        Serial.println("C6 firmware update reported success. Restarting the P4...");
        delay(1500);
        ESP.restart();
    }

    Serial.println("C6 updater did not report success.");
    Serial.println("Inspect the messages above; Wi-Fi may still work with the existing C6 firmware.");
}

void loop() {
    delay(1000);
}
