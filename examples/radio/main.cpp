
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <esp_heap_caps.h>

#include "Audio.h"
#include "softcodecs/AudioSourceStream.h"
#include "secrets.h"

//const char TAG[] = "MAIN";
const char* TAG = nullptr;


constexpr char STREAM_URL[] = "http://mp3.ffh.de/radioffh/hqlivestream.aac";

// Weitere Teststreams:
//constexpr char STREAM_URL[] = "https://st01.sslstream.dlf.de/dlf/01/128/mp3/stream.mp3";
// constexpr char STREAM_URL[] = "https://st01.sslstream.dlf.de/dlf/01/mid/aac/stream.aac";
// constexpr char STREAM_URL[] = "https://st01.sslstream.dlf.de/dlf/01/high/aac/stream.aac";
// constexpr char STREAM_URL[] = "https://wdr-wdr2-rheinruhr.icecastssl.wdr.de/wdr/wdr2/rheinruhr/mp3/128/stream.mp3";

AudioPlayer *selectedPlayer = nullptr;
AudioSourceStream audioSource;

WiFiClient plainClient;
WiFiClientSecure tlsClient;
HTTPClient http;

AudioOutputI2S i2s({
    PIN_I2S_BLCK, // BCLK
    PIN_I2S_WS,   // WS / LRCLK
    PIN_I2S_DOUT, // DOUT
    PIN_I2S_MLCK  // MCLK
});

#if defined(AUDIO_CODEC_ES8388)
AudioControlES8388 codec({PIN_I2C_SDA, PIN_I2C_SCL, PIN_AMPLIFIER, PIN_AMPLIFIER_ACTIVE});
#elif defined(AUDIO_CODEC_ES8311)
AudioControlES8311 codec({PIN_I2C_SDA, PIN_I2C_SCL, PIN_AMPLIFIER, PIN_AMPLIFIER_ACTIVE});
#else
#error "AUDIO_CODEC not defined"
#endif

AudioConnection *patchCordLeft = nullptr;
AudioConnection *patchCordRight = nullptr;

// ============================================================================
// Error handling
// ============================================================================

[[noreturn]] void stopWithError(const char *message) {
    ESP_LOGE(TAG, "%s", message);

    while (true) {
        delay(1000);
    }
}

// ============================================================================
// I2S sample-rate handling
// ============================================================================

bool onStreamSampleRate(uint32_t rate, void *) {
    ESP_LOGI(TAG, "Switching output sample rate to %u Hz", static_cast<unsigned>(rate));

    return i2s.setSampleRate(static_cast<float>(rate));
}

// ============================================================================
// Player selection
// ============================================================================

// The radio application currently supports MP3 and AAC streams.
// Both players share AudioDecoderStream, so they support dynamic
// sample-rate negotiation.
//
// WAV is deliberately not included here: AudioPlaySdWav currently
// requires a seekable source, whereas AudioSourceStream is non-seekable.

AudioPlayer *findPlayer(const char *contentType) {
    static AudioPlayMp3 mp3;
    static AudioPlayAac aac;

    // Configure both candidates through their common decoder interface.
    mp3.onSampleRateChange(onStreamSampleRate);
    aac.onSampleRateChange(onStreamSampleRate);

    AudioPlayer *candidates[] = {&mp3, &aac};

    for (AudioPlayer *candidate : candidates) {
        if (candidate->matchesContentType(contentType)) {
            ESP_LOGI(TAG, "Selected player: %s", candidate->name());
            return candidate;
        }
    }

    ESP_LOGE(TAG, "Unsupported stream content type: %s", contentType ? contentType : "(null)");

    return nullptr;
}

// ============================================================================
// Audio connections
// ============================================================================

bool connectPlayer(AudioPlayer &player) {
    patchCordLeft = new AudioConnection();
    patchCordRight = new AudioConnection();

    if (patchCordLeft == nullptr || patchCordRight == nullptr) {
        ESP_LOGE(TAG, "Could not allocate audio connections");
        return false;
    }

    const int leftResult = patchCordLeft->connect(player, 0, i2s, 0);

    if (leftResult != 0) {
        ESP_LOGE(TAG, "Left audio connection failed (%d)", leftResult);
        return false;
    }

    const int rightResult = patchCordRight->connect(player, 1, i2s, 1);

    if (rightResult != 0) {
        ESP_LOGE(TAG, "Right audio connection failed (%d)", rightResult);
        return false;
    }

    return true;
}

// ============================================================================
// HTTP
// ============================================================================
bool beginStreamConnection() {
    if (strncasecmp(STREAM_URL, "https://", 8) == 0) {
        // HTTPS: TLS verwenden, Zertifikatsprüfung deaktiviert.
        tlsClient.setInsecure();
        return http.begin(tlsClient, STREAM_URL);
    }

    if (strncasecmp(STREAM_URL, "http://", 7) == 0) {
        // HTTP: normale TCP-Verbindung ohne TLS.
        return http.begin(plainClient, STREAM_URL);
    }

    ESP_LOGE(TAG, "Unsupported URL scheme: %s", STREAM_URL);
    return false;
}

void configStream(HTTPClient &client, bool useIcy = false, followRedirects_t redirects = HTTPC_STRICT_FOLLOW_REDIRECTS,
                  int redirectLimit = 5, int timeout = 15000) {
    client.useHTTP10(true);
    client.setUserAgent("VLC/3.0.21");
    client.addHeader("Accept", "*/*");

    const char *headerKeys[] = {"content-type", "icy-metaint"};

    client.collectHeaders(headerKeys, sizeof(headerKeys) / sizeof(headerKeys[0]));

    if (useIcy) {
        client.addHeader("Icy-MetaData", "1");
    }

    client.setTimeout(timeout);
    client.setFollowRedirects(redirects);
    client.setRedirectLimit(redirectLimit);
}

// ============================================================================
// Diagnostics
// ============================================================================

void printFreeRam() {
    ESP_LOGI(TAG, "Free heap: internal %u B (largest block %u B, min ever %u B), PSRAM %u B",
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
             static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
}

void printStatus(AudioPlayer &player) {
    static uint32_t lastStatus = 0;

    if (millis() - lastStatus >= 5000) {
        lastStatus = millis();

        if (player.hasLastError()) {
            ESP_LOGI(TAG, "Wi-Fi: %s (RSSI %d dBm), %s: %s (error %d)",
                     WiFi.status() == WL_CONNECTED ? "connected" : "disconnected", WiFi.RSSI(), player.name(),
                     player.isPlaying() ? "playing" : "waiting", player.lastError());
        } else {
            ESP_LOGI(TAG, "Wi-Fi: %s (RSSI %d dBm), %s: %s",
                     WiFi.status() == WL_CONNECTED ? "connected" : "disconnected", WiFi.RSSI(), player.name(),
                     player.isPlaying() ? "playing" : "waiting");
        }

#if SOFTCODEC_METRICS
        if (player.hasDecoderMetrics()) {
            ESP_LOGI(TAG, "Decoder load: avg %.2f%%, max %.2f%%", player.decodeProcessorUsage(),
                     player.decodeProcessorUsageMax());
        }
#endif
    }
}


// ============================================================================
// Setup
// ============================================================================

void setup() {
    Serial.begin(115200);
    delay(1000);

    i2s.setChannelCount(codec.channels()); //i.e.s es8311 is mono
    AudioMemory(10);

    ESP_LOGI(TAG, "Initializing codec...");

    if (!codec.enable()) {
        stopWithError("Audio codec initialization failed");
    }

    codec.volume(0.6f);

    // Wi-Fi
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);

    ESP_LOGI(TAG, "Connecting to Wi-Fi");

    while (WiFi.status() != WL_CONNECTED) {
        delay(200);
        Serial.print('.');
    }

    ESP_LOGI(TAG, "\nConnected, IP: %s", WiFi.localIP().toString().c_str());


    configStream(http, true);
    if (!beginStreamConnection()) {
        stopWithError("ERROR: Could not initialize HTTP connection");
    }

    const int responseCode = http.GET();

    if (responseCode != HTTP_CODE_OK) {
        ESP_LOGE(TAG, "HTTP request failed: %d", responseCode);
        http.end();
        stopWithError("Stream request failed");
    }

    // Select the appropriate player using its own MIME-type information.
    String contentType = http.header("content-type");
    contentType.trim();

    selectedPlayer = findPlayer(contentType.c_str());

    if (selectedPlayer == nullptr) {
        http.end();
        stopWithError("Could not find a compatible audio player");
    }

    // From this point onward, use only the common player interface.
    AudioPlayer &player = *selectedPlayer;

    ESP_LOGI(TAG, "Player: %s, Content-Type: %s", player.name(), contentType.c_str());

    if (!connectPlayer(player)) {
        http.end();
        stopWithError("Could not connect player to I2S output");
    }

    // Start the buffered source.
    if (!audioSource.open(http)) {
        http.end();
        stopWithError("Could not start stream buffer");
    }

    ESP_LOGI(TAG, "Buffering stream...");

    constexpr size_t PREBUFFER_BYTES = 32U * 1024U;
    constexpr uint32_t PREBUFFER_TIMEOUT_MS = 30000;

    const uint32_t bufferDeadline = millis() + PREBUFFER_TIMEOUT_MS;

    while (audioSource.bufferedBytes() < PREBUFFER_BYTES && static_cast<int32_t>(bufferDeadline - millis()) > 0) {
        delay(10);
    }

    if (audioSource.bufferedBytes() < PREBUFFER_BYTES) {
        audioSource.close();
        http.end();
        stopWithError("Stream prebuffer timed out");
    }

    // No codec-specific branch required.
    if (!player.play(audioSource)) {
        audioSource.close();
        http.end();
        stopWithError("Stream playback could not be started");
    }

    ESP_LOGI(TAG, "Playback started: %s", player.name());

    printFreeRam();
}


// ============================================================================
// Main loop
// ============================================================================

void loop() {
    static char icyMetadata[AudioSourceStream::ICY_STREAMTITLE_MAX_SIZE];

    AudioPlayer &player = *selectedPlayer;
    printStatus(player);
    const int metadataLength = audioSource.takeIcyStreamTitle(icyMetadata, sizeof(icyMetadata));
    if (metadataLength > 0) {
        ESP_LOGI(TAG, "ICY metadata: %s", icyMetadata);
    }


    vTaskDelay(pdMS_TO_TICKS(200));
}
