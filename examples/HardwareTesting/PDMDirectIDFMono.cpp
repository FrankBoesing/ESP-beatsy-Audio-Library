/*
 * Direct ESP-IDF PDM-DAC MONO test for ESP32-S3.
 *
 * This deliberately bypasses AudioStream and AudioOutputPDM.
 * One DATA line only: GPIO 43.
 * Use the same filter/amplifier setup as the successful stereo test.
 */

#include <Arduino.h>
#include <cmath>
#include <driver/i2s_common.h>
#include <driver/i2s_pdm.h>
#include <esp_err.h>
#include <soc/soc_caps.h>

#if !defined(SOC_I2S_SUPPORTS_PCM2PDM) || !SOC_I2S_SUPPORTS_PCM2PDM
#error "This test requires hardware PCM-to-PDM support."
#endif

#if !defined(SOC_I2S_HW_VERSION_2) || !SOC_I2S_HW_VERSION_2
#error "This test requires I2S HW version 2 (e.g. ESP32-S3) for PDM DAC mode."
#endif

namespace {
constexpr gpio_num_t PIN_PDM_DATA = GPIO_NUM_43;
constexpr uint32_t SAMPLE_RATE = 44100;
constexpr float TONE_HZ = 1000.0f;
constexpr float AMPLITUDE = 0.20f;

// 441 mono PCM samples at 44.1 kHz contain exactly 10 cycles at 1 kHz.
constexpr size_t SAMPLES_PER_BUFFER = 441;
int16_t pcmBuffer[SAMPLES_PER_BUFFER];

i2s_chan_handle_t txHandle = nullptr;
uint32_t successfulWrites = 0;
uint32_t failedWrites = 0;

void stopWithError(const char *operation, esp_err_t err) {
    Serial.printf("%s failed: %s (0x%x)\n",
                  operation, esp_err_to_name(err), static_cast<unsigned>(err));
    Serial.println("PDM mono test stopped.");
    while (true) delay(1000);
}

void fillSineBuffer() {
    for (size_t i = 0; i < SAMPLES_PER_BUFFER; ++i) {
        const float phase = 2.0f * PI * TONE_HZ * static_cast<float>(i) /
                            static_cast<float>(SAMPLE_RATE);
        pcmBuffer[i] = static_cast<int16_t>(
            AMPLITUDE * 32767.0f * sinf(phase));
    }
}

void startPdmMono() {
    i2s_chan_config_t channelConfig =
        I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    channelConfig.auto_clear_after_cb = true;

    esp_err_t err = i2s_new_channel(&channelConfig, &txHandle, nullptr);
    if (err != ESP_OK) stopWithError("i2s_new_channel", err);

    i2s_pdm_tx_config_t pdmConfig = {};
    pdmConfig.clk_cfg = I2S_PDM_TX_CLK_DAC_DEFAULT_CONFIG(SAMPLE_RATE);
    pdmConfig.slot_cfg = I2S_PDM_TX_SLOT_PCM_FMT_DAC_DEFAULT_CONFIG(
        I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO);

    pdmConfig.gpio_cfg.clk = I2S_GPIO_UNUSED;
    pdmConfig.gpio_cfg.dout = PIN_PDM_DATA;
    pdmConfig.gpio_cfg.dout2 = I2S_GPIO_UNUSED;
    pdmConfig.gpio_cfg.invert_flags.clk_inv = false;

    err = i2s_channel_init_pdm_tx_mode(txHandle, &pdmConfig);
    if (err != ESP_OK) stopWithError("i2s_channel_init_pdm_tx_mode", err);

    err = i2s_channel_enable(txHandle);
    if (err != ESP_OK) stopWithError("i2s_channel_enable", err);
}
} // namespace

void setup() {
    Serial.begin(115200);
    delay(500);

    Serial.println();
    Serial.println("Direct ESP-IDF PDM-DAC MONO test");
    Serial.printf("DATA: GPIO %d\n", static_cast<int>(PIN_PDM_DATA));
    Serial.printf("PCM: %lu Hz, sine: %.0f Hz, amplitude: %.2f\n",
                  static_cast<unsigned long>(SAMPLE_RATE), TONE_HZ, AMPLITUDE);

    fillSineBuffer();
    startPdmMono();

    Serial.println("PDM mono TX running. AudioStream/library path is not used.");
}

void loop() {
    size_t bytesWritten = 0;
    const esp_err_t err = i2s_channel_write(
        txHandle, pcmBuffer, sizeof(pcmBuffer), &bytesWritten, 1000);

    if (err == ESP_OK && bytesWritten == sizeof(pcmBuffer)) {
        ++successfulWrites;
    } else {
        ++failedWrites;
        Serial.printf("i2s_channel_write: %s, wrote %u/%u bytes\n",
                      esp_err_to_name(err),
                      static_cast<unsigned>(bytesWritten),
                      static_cast<unsigned>(sizeof(pcmBuffer)));
    }

    static uint32_t lastReportMs = 0;
    const uint32_t now = millis();
    if (now - lastReportMs >= 1000) {
        lastReportMs = now;
        Serial.printf("PDM mono active; writes OK=%lu failed=%lu\n",
                      static_cast<unsigned long>(successfulWrites),
                      static_cast<unsigned long>(failedWrites));
    }
}
