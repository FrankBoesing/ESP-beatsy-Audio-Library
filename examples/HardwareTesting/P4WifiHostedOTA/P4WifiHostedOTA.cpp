// Update the ESP32-C6 ESP-Hosted slave firmware from the P4 over Wi-Fi.
//
// Build a matching ESP-Hosted v2.13.0 slave image first (see
// docs/esp32-p4-wifi6-wifi.md). Copy the resulting network_adapter.bin to a
// folder served by a temporary HTTP server on the same trusted LAN.
// Define WIFI_SSID, WIFI_PASS and HOSTED_FW_URL in local src/secrets.h.
//
// HOSTED_FW_URL should look like:
//   "http://192.168.178.10:8000/network_adapter.bin"
//
// This example uses the core's Hosted OTA primitives directly. The upstream
// ESP_HostedOTA helper hard-codes a URL whose v2.13.0 file is not published,
// and its HTTPClient/NetworkClient cleanup can fault on the error path.

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <NetworkClient.h>
#include <esp32-hal-hosted.h>
#include <esp_system.h>

#include "secrets.h"

#ifndef HOSTED_FW_URL
#error "Define HOSTED_FW_URL in local src/secrets.h"
#endif

static constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS = 30000;
static constexpr uint32_t DOWNLOAD_IDLE_TIMEOUT_MS = 30000;
static constexpr size_t OTA_BUFFER_SIZE = 2048;

static bool updateHostedFirmware(const char *url) {
    if (!hostedIsInitialized()) {
        Serial.println("ERROR: ESP-Hosted is not initialized.");
        return false;
    }

    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("ERROR: Wi-Fi is not connected.");
        return false;
    }

    // Declaration order matters: HTTPClient is destroyed before its client.
    NetworkClient networkClient;
    HTTPClient http;

    Serial.printf("Downloading C6 firmware from: %s\n", url);
    if (!http.begin(networkClient, url)) {
        Serial.println("ERROR: HTTPClient begin failed.");
        return false;
    }

    const int httpCode = http.GET();
    if (httpCode != HTTP_CODE_OK) {
        Serial.printf("ERROR: Firmware download returned HTTP %d.\n", httpCode);
        http.end();
        return false;
    }

    const int imageSize = http.getSize();
    if (imageSize <= 0) {
        Serial.printf("ERROR: Invalid Content-Length: %d.\n", imageSize);
        http.end();
        return false;
    }

    NetworkClient *stream = http.getStreamPtr();
    if (!stream) {
        Serial.println("ERROR: No HTTP response stream.");
        http.end();
        return false;
    }

    uint8_t *buffer = (uint8_t *)malloc(OTA_BUFFER_SIZE);
    if (!buffer) {
        Serial.println("ERROR: Could not allocate OTA buffer.");
        http.end();
        return false;
    }

    if (!hostedBeginUpdate()) {
        Serial.println("ERROR: ESP-Hosted OTA begin failed.");
        free(buffer);
        http.end();
        return false;
    }

    size_t remaining = (size_t)imageSize;
    uint32_t lastDataTime = millis();
    size_t lastReported = remaining;
    bool ok = true;

    while (remaining > 0) {
        const int available = stream->available();
        if (available <= 0) {
            if (!http.connected() ||
                (uint32_t)(millis() - lastDataTime) >= DOWNLOAD_IDLE_TIMEOUT_MS) {
                Serial.printf("\nERROR: Download stopped with %u bytes remaining.\n",
                              (unsigned int)remaining);
                ok = false;
                break;
            }
            delay(1);
            continue;
        }

        size_t chunkSize = (size_t)available;
        if (chunkSize > OTA_BUFFER_SIZE) {
            chunkSize = OTA_BUFFER_SIZE;
        }
        if (chunkSize > remaining) {
            chunkSize = remaining;
        }

        const int readLen = stream->readBytes(buffer, chunkSize);
        if (readLen <= 0) {
            Serial.println("\nERROR: Reading the firmware stream failed.");
            ok = false;
            break;
        }

        if (!hostedWriteUpdate(buffer, (uint32_t)readLen)) {
            Serial.println("\nERROR: Writing firmware to the C6 failed.");
            ok = false;
            break;
        }

        remaining -= (size_t)readLen;
        lastDataTime = millis();

        if (lastReported - remaining >= 64 * 1024 || remaining == 0) {
            Serial.printf("\rTransferred %u / %u bytes",
                          (unsigned int)((size_t)imageSize - remaining),
                          (unsigned int)imageSize);
            lastReported = remaining;
        }
    }
    Serial.println();

    free(buffer);

    if (ok && !hostedEndUpdate()) {
        Serial.println("ERROR: Finalizing C6 firmware failed.");
        ok = false;
    }

    if (ok && !hostedActivateUpdate()) {
        Serial.println("ERROR: Activating C6 firmware failed.");
        ok = false;
    }

    // Keep NetworkClient alive until HTTPClient has released its reference.
    http.end();

    if (ok) {
        Serial.println("SUCCESS: ESP-Hosted C6 firmware update completed.");
    } else {
        Serial.println("C6 firmware update failed. Do not assume the image was activated.");
    }

    return ok;
}

void setup() {
    Serial.begin(115200);
    delay(1000);

    Serial.println();
    Serial.println("ESP-beatsy: ESP32-P4-WIFI6 custom C6 Hosted firmware updater");
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
        return;
    }

    Serial.printf("Connected. IP: %s\n", WiFi.localIP().toString().c_str());
    Serial.printf("RSSI: %d dBm\n", WiFi.RSSI());
    Serial.println("Updating the C6 with the explicitly configured firmware image.");
    Serial.println("Do not power off the board during the update.");

    if (updateHostedFirmware(HOSTED_FW_URL)) {
        Serial.println("Restarting the P4 to reconnect to the updated C6...");
        delay(1500);
        ESP.restart();
    }

    Serial.println("Updater returned without success; inspect errors above.");
}

void loop() {
    delay(1000);
}
