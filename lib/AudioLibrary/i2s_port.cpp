#include "i2s_port.h"

#include <cmath>
#include <atomic>
#include <esp_err.h>
#include <esp_clk_tree.h>
#include <hal/i2s_hal.h>
#include <hal/i2s_ll.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <driver/i2s_pdm.h>
#include "AudioStream.h"

namespace {
using Direction = AudioI2SPort::Direction;
using Pins = AudioI2SPort::Pins;
using Format = AudioI2SPort::Format;
using EventCallback = AudioI2SPort::EventCallback;

constexpr char TAG[] = "AudioI2SPort";

// Reconfiguration pauses graph traversal while clocks and channel states
// are changed. AudioStream's mutex is recursive, so nested sample-rate calls
// remain safe.
class AudioGraphUpdateLock {
  public:
    AudioGraphUpdateLock() { AudioNoInterrupts(); }
    ~AudioGraphUpdateLock() { AudioInterrupts(); }

    AudioGraphUpdateLock(const AudioGraphUpdateLock &) = delete;
    AudioGraphUpdateLock &operator=(const AudioGraphUpdateLock &) = delete;
};
constexpr uint16_t DMA_DESC_NUM = 6;
constexpr uint16_t DMA_FRAME_NUM = AUDIO_BLOCK_SAMPLES;
constexpr uint8_t MAX_PORTS = AudioI2SPort::MAX_CONTROLLERS;

uint32_t choosePdmDacBclkDivider(uint32_t rate, i2s_clock_src_t clockSource) {
    constexpr uint32_t DEFAULT_BCLK_DIV = 13;
    constexpr uint32_t MIN_BCLK_DIV = 8;
    constexpr uint32_t MAX_BCLK_DIV = 255;
    constexpr uint32_t PDM_BCLK_FACTOR = 64;
    constexpr uint32_t PDM_OVERSAMPLE_RATIO = 2; // fp=960, fs=480

    if (rate == 0) return DEFAULT_BCLK_DIV;

    uint32_t sourceHz = 0;
    const auto source = static_cast<soc_module_clk_t>(clockSource);
    if (esp_clk_tree_src_get_freq_hz(
            source, ESP_CLK_TREE_SRC_FREQ_PRECISION_CACHED, &sourceHz) != ESP_OK ||
        sourceHz == 0) {
        // Keep the IDF default if the clock tree cannot report the source.
        return DEFAULT_BCLK_DIV;
    }

    // ESP-IDF 5.5's ESP32-S3 PDM driver replaces its fractional MCLK divider
    // with an integer divider to reduce PDM noise. Choose BCLK_DIV to minimize
    // the resulting PCM sample-rate error while keeping fp/fs = 2.
    uint32_t bestDivider = DEFAULT_BCLK_DIV;
    uint32_t bestDistance = MAX_BCLK_DIV;
    double bestError = INFINITY;

    for (uint32_t bclkDiv = MIN_BCLK_DIV; bclkDiv <= MAX_BCLK_DIV; ++bclkDiv) {
        const uint64_t targetMclk =
            static_cast<uint64_t>(rate) * PDM_BCLK_FACTOR *
            PDM_OVERSAMPLE_RATIO * bclkDiv;
        const uint32_t mclkDiv = static_cast<uint32_t>(sourceHz / targetMclk);
        if (mclkDiv < 2 || mclkDiv >= 256) continue;
        if (static_cast<double>(sourceHz) <= static_cast<double>(targetMclk) * 1.99) continue;

        const uint64_t totalDivider = static_cast<uint64_t>(mclkDiv) *
            PDM_BCLK_FACTOR * PDM_OVERSAMPLE_RATIO * bclkDiv;
        const double actualRate = static_cast<double>(sourceHz) / totalDivider;
        const double error = std::fabs(actualRate - static_cast<double>(rate));
        const uint32_t distance = bclkDiv > DEFAULT_BCLK_DIV
            ? bclkDiv - DEFAULT_BCLK_DIV
            : DEFAULT_BCLK_DIV - bclkDiv;

        if (error < bestError - 1e-9 ||
            (std::fabs(error - bestError) <= 1e-9 && distance < bestDistance)) {
            bestError = error;
            bestDivider = bclkDiv;
            bestDistance = distance;
        }
    }

    return bestDivider;
}

i2s_pdm_tx_clk_config_t makePdmDacClockConfig(uint32_t rate) {
    i2s_pdm_tx_clk_config_t cfg = I2S_PDM_TX_CLK_DAC_DEFAULT_CONFIG(rate);
    // Preserve the tested DAC mode: fp=960, fs=480, OSR=2.
    cfg.up_sample_fs = 480;
    cfg.bclk_div = choosePdmDacBclkDivider(rate, cfg.clk_src);
    return cfg;
}

#if defined(SOC_I2S_SUPPORTS_PCM2PDM) && SOC_I2S_SUPPORTS_PCM2PDM && \
    defined(SOC_I2S_HW_VERSION_2) && SOC_I2S_HW_VERSION_2
void applyPrecisePdmMclkIfNeeded(const i2s_pdm_tx_clk_config_t &cfg) {
    constexpr uint32_t PDM_BCLK_FACTOR = 64;

    const uint32_t rate = cfg.sample_rate_hz;
    const uint32_t oversampleRatio =
        cfg.up_sample_fs != 0 ? cfg.up_sample_fp / cfg.up_sample_fs : 0;
    if (rate == 0 || oversampleRatio == 0 || cfg.bclk_div < 8) {
        ESP_LOGW(TAG, "Skipping experimental PDM MCLK correction: invalid clock config");
        return;
    }

    uint32_t sourceHz = 0;
    const auto source = static_cast<soc_module_clk_t>(cfg.clk_src);
    if (esp_clk_tree_src_get_freq_hz(
            source, ESP_CLK_TREE_SRC_FREQ_PRECISION_CACHED, &sourceHz) != ESP_OK ||
        sourceHz == 0) {
        ESP_LOGW(TAG, "Skipping experimental PDM MCLK correction: source clock unavailable");
        return;
    }

    const uint64_t mclk64 = static_cast<uint64_t>(rate) * PDM_BCLK_FACTOR *
                            oversampleRatio * cfg.bclk_div;
    if (mclk64 == 0 || mclk64 > 0xFFFFFFFFULL) {
        ESP_LOGW(TAG, "Skipping experimental PDM MCLK correction: target MCLK out of range");
        return;
    }
    const uint32_t targetMclk = static_cast<uint32_t>(mclk64);
    const uint32_t integerDivider = sourceHz / targetMclk;
    if (integerDivider < 2 || integerDivider >= 256 ||
        static_cast<double>(sourceHz) <= static_cast<double>(targetMclk) * 1.99) {
        ESP_LOGW(TAG, "Skipping experimental PDM MCLK correction: invalid integer divider");
        return;
    }

    const uint64_t rateDivider = static_cast<uint64_t>(integerDivider) *
                                 PDM_BCLK_FACTOR * oversampleRatio * cfg.bclk_div;
    const double integerRate = static_cast<double>(sourceHz) / rateDivider;
    const double errorPercent =
        100.0 * std::fabs(integerRate - static_cast<double>(rate)) / rate;

    // Keep the ESP-IDF low-noise integer divider when its rate error is already
    // small. This leaves the currently working 48 kHz configuration untouched.
    constexpr double MAX_INTEGER_RATE_ERROR_PERCENT = 0.5;
    if (errorPercent <= MAX_INTEGER_RATE_ERROR_PERCENT) {
        ESP_LOGI(TAG,
                 "PDM clock %lu Hz: keep IDF integer divider; predicted rate %.2f Hz (error %.3f%%)",
                 static_cast<unsigned long>(rate), integerRate, errorPercent);
        return;
    }

    // Diagnostic experiment: ESP-IDF 5.5 computes a fractional divider and
    // then overwrites it with an integer-only divider to reduce PDM noise.
    // Restore the HAL-calculated fractional divider for rates where that
    // integer-only divider would cause a significant sample-rate error.
    hal_utils_clk_div_t divider = {};
    i2s_hal_calc_mclk_precise_division(sourceHz, targetMclk, &divider);
    if (divider.integer < 2 || divider.denominator == 0) {
        ESP_LOGW(TAG, "Could not calculate fractional PDM MCLK divider for %lu Hz",
                 static_cast<unsigned long>(rate));
        return;
    }

    i2s_dev_t *hw = I2S_LL_GET_HW(0);
    i2s_ll_tx_set_mclk(hw, &divider);
    i2s_ll_tx_update(hw);

    const uint64_t dividerNumerator =
        static_cast<uint64_t>(divider.integer) * divider.denominator + divider.numerator;
    const double actualMclk = static_cast<double>(sourceHz) * divider.denominator /
                              static_cast<double>(dividerNumerator);
    const double predictedRate =
        actualMclk / (PDM_BCLK_FACTOR * oversampleRatio * cfg.bclk_div);
    ESP_LOGW(TAG,
             "EXPERIMENTAL fractional PDM MCLK: source=%lu Hz, requested=%lu Hz, BCLK divider=%lu, "
             "integer-divider estimate=%.2f Hz, divider=%lu+%lu/%lu, predicted=%.2f Hz. "
             "Listen for added PDM noise.",
             static_cast<unsigned long>(sourceHz), static_cast<unsigned long>(rate),
             static_cast<unsigned long>(cfg.bclk_div), integerRate,
             static_cast<unsigned long>(divider.integer),
             static_cast<unsigned long>(divider.numerator),
             static_cast<unsigned long>(divider.denominator), predictedRate);
}
#else
void applyPrecisePdmMclkIfNeeded(const i2s_pdm_tx_clk_config_t &) {}
#endif

struct PortState {
    i2s_chan_handle_t txHandle = nullptr;
    i2s_chan_handle_t rxHandle = nullptr;
    Pins pins = {-1, -1, -1, -1, -1};
    Format format = {};
    Direction physicalMaster = Direction::None;
    uint32_t sampleRate = 0;
    bool pairCreated = false;
    bool txAcquired = false;
    bool rxAcquired = false;
    bool txActive = false;
    bool rxActive = false;
    bool txEnabled = false;
    bool rxEnabled = false;

    portMUX_TYPE callbackMux = portMUX_INITIALIZER_UNLOCKED;
    EventCallback txCallback = nullptr;
    void *txContext = nullptr;
    EventCallback rxCallback = nullptr;
    void *rxContext = nullptr;
    uint32_t txCallbacksInFlight = 0;
    uint32_t rxCallbacksInFlight = 0;
};

PortState ports[MAX_PORTS]{};
// AudioStream has one graph clock, irrespective of how many I2S controllers run.
// A single task-side writer publishes the owner to the I2S callback ISRs.
std::atomic<int32_t> schedulerOwner{-1};

int8_t getSchedulerOwner() { return static_cast<int8_t>(schedulerOwner.load(std::memory_order_acquire)); }
void setSchedulerOwner(int8_t owner) { schedulerOwner.store(static_cast<int32_t>(owner), std::memory_order_release); }
PortState *stateFor(uint8_t controller) { return controller < MAX_PORTS ? &ports[controller] : nullptr; }
uint8_t controllerOf(const PortState *state) { return static_cast<uint8_t>(state - ports); }

int8_t ownerCode(uint8_t controller, Direction direction) {
    if (direction == Direction::None || controller >= MAX_PORTS) return -1;
    return static_cast<int8_t>(controller * 2U + (direction == Direction::Rx ? 1U : 0U));
}

bool sameFormat(const Format &a, const Format &b) {
    return a.mode == b.mode && a.encoding == b.encoding && a.slots == b.slots && a.slotBits == b.slotBits &&
           a.rateMultiplier == b.rateMultiplier;
}

bool formatImplemented(const Format &f) {
    if (f.slotBits != 16) return false;

    if (f.mode == AudioI2SPort::PeripheralMode::PDM) {
        return f.encoding == AudioI2SPort::DataEncoding::PCM && f.rateMultiplier == 1 &&
               (f.slots == 1 || f.slots == 2);
    }

    if (f.mode != AudioI2SPort::PeripheralMode::Standard || f.slots != 2) return false;

    if (f.encoding == AudioI2SPort::DataEncoding::PCM) return f.rateMultiplier == 1;

    // Software BMC stream: at 4x PCM frame rate, stereo 16-bit I2S serializes
    // 128 BMC half-bit states for each stereo PCM frame.
    return f.encoding == AudioI2SPort::DataEncoding::SpdifBMC && f.rateMultiplier == 4;
}

bool sameOptionalPin(int8_t requested, int8_t configured) { return requested < 0 || requested == configured; }

OSIZE
i2s_std_config_t makeStdConfig(const Pins &pins, uint32_t rate, bool transmit, bool master, const Format &format) {
    i2s_std_config_t cfg = {};
    cfg.clk_cfg.sample_rate_hz = rate * format.rateMultiplier;
    cfg.clk_cfg.clk_src = I2S_CLK_SRC_DEFAULT;
    // For SPDIF BMC the serializer rate is 4x Fs; 128x that physical frame
    // rate gives an MCLK of 512x Fs and an integer MCLK/BCLK ratio of 4.
    cfg.clk_cfg.mclk_multiple = format.encoding == AudioI2SPort::DataEncoding::SpdifBMC
                                   ? I2S_MCLK_MULTIPLE_128
                                   : I2S_MCLK_MULTIPLE_256;
    // S/PDIF is already a complete serialized BMC bitstream. Start with the
    // usual stereo slot settings and disable the Philips one-bit data shift
    // for BMC. The IDF helper macros expand to brace initializers, so they
    // cannot be used as operands of a conditional expression.
    i2s_std_slot_config_t slotCfg =
        I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
    if (format.encoding == AudioI2SPort::DataEncoding::SpdifBMC) {
        slotCfg.bit_shift = false;
    }
    cfg.slot_cfg = slotCfg;

    cfg.gpio_cfg.mclk = master && pins.mclk >= 0 ? static_cast<gpio_num_t>(pins.mclk) : I2S_GPIO_UNUSED;
    cfg.gpio_cfg.bclk = pins.bclk >= 0 ? static_cast<gpio_num_t>(pins.bclk) : I2S_GPIO_UNUSED;
    cfg.gpio_cfg.ws = pins.ws >= 0 ? static_cast<gpio_num_t>(pins.ws) : I2S_GPIO_UNUSED;
    cfg.gpio_cfg.dout = transmit && pins.dout >= 0 ? static_cast<gpio_num_t>(pins.dout) : I2S_GPIO_UNUSED;
    cfg.gpio_cfg.din = !transmit && pins.din >= 0 ? static_cast<gpio_num_t>(pins.din) : I2S_GPIO_UNUSED;
    cfg.gpio_cfg.invert_flags.mclk_inv = false;
    cfg.gpio_cfg.invert_flags.bclk_inv = false;
    cfg.gpio_cfg.invert_flags.ws_inv = false;
    return cfg;
}

OSIZE
i2s_std_gpio_config_t makeGpioConfig(const Pins &pins, bool transmit, bool master) {
    i2s_std_gpio_config_t cfg = {};
    cfg.mclk = master && pins.mclk >= 0 ? static_cast<gpio_num_t>(pins.mclk) : I2S_GPIO_UNUSED;
    cfg.bclk = pins.bclk >= 0 ? static_cast<gpio_num_t>(pins.bclk) : I2S_GPIO_UNUSED;
    cfg.ws = pins.ws >= 0 ? static_cast<gpio_num_t>(pins.ws) : I2S_GPIO_UNUSED;
    cfg.dout = transmit && pins.dout >= 0 ? static_cast<gpio_num_t>(pins.dout) : I2S_GPIO_UNUSED;
    cfg.din = !transmit && pins.din >= 0 ? static_cast<gpio_num_t>(pins.din) : I2S_GPIO_UNUSED;
    cfg.invert_flags.mclk_inv = false;
    cfg.invert_flags.bclk_inv = false;
    cfg.invert_flags.ws_inv = false;
    return cfg;
}

OSIZE
void resetPortState(PortState &s) {
    s.txHandle = s.rxHandle = nullptr;
    s.pins = {-1, -1, -1, -1, -1};
    s.format = {};
    s.physicalMaster = Direction::None;
    s.sampleRate = 0;
    s.pairCreated = false;
    s.txAcquired = s.rxAcquired = false;
    s.txActive = s.rxActive = false;
    s.txEnabled = s.rxEnabled = false;
    portENTER_CRITICAL(&s.callbackMux);
    s.txCallback = s.rxCallback = nullptr;
    s.txContext = s.rxContext = nullptr;
    s.txCallbacksInFlight = 0;
    s.rxCallbacksInFlight = 0;
    portEXIT_CRITICAL(&s.callbackMux);
}

bool AUDIO_I2S_ISR_ATTR dispatchTx(i2s_chan_handle_t channel, i2s_event_data_t *event, void *context) {
    auto *s = static_cast<PortState *>(context);
    if (!s) return false;
    EventCallback callback;
    void *userContext;
    portENTER_CRITICAL_ISR(&s->callbackMux);
    ++s->txCallbacksInFlight;
    callback = s->txCallback;
    userContext = s->txContext;
    portEXIT_CRITICAL_ISR(&s->callbackMux);
    const bool taskWoken = callback ? callback(channel, event, userContext) : false;
    portENTER_CRITICAL_ISR(&s->callbackMux);
    --s->txCallbacksInFlight;
    portEXIT_CRITICAL_ISR(&s->callbackMux);
    return taskWoken;
}

bool AUDIO_I2S_ISR_ATTR dispatchRx(i2s_chan_handle_t channel, i2s_event_data_t *event, void *context) {
    auto *s = static_cast<PortState *>(context);
    if (!s) return false;
    EventCallback callback;
    void *userContext;
    portENTER_CRITICAL_ISR(&s->callbackMux);
    ++s->rxCallbacksInFlight;
    callback = s->rxCallback;
    userContext = s->rxContext;
    portEXIT_CRITICAL_ISR(&s->callbackMux);
    const bool taskWoken = callback ? callback(channel, event, userContext) : false;
    portENTER_CRITICAL_ISR(&s->callbackMux);
    --s->rxCallbacksInFlight;
    portEXIT_CRITICAL_ISR(&s->callbackMux);
    return taskWoken;
}

OSIZE
bool createPair(PortState &s, const Pins &requested, Direction first, uint32_t rate, const Format &format) {
    const bool spdifBmc = format.encoding == AudioI2SPort::DataEncoding::SpdifBMC;
    const bool pdmDac = format.mode == AudioI2SPort::PeripheralMode::PDM;
    if ((!spdifBmc && !pdmDac && (requested.bclk < 0 || requested.ws < 0)) ||
        (first == Direction::Tx && requested.dout < 0) ||
        (first == Direction::Rx && requested.din < 0) ||
        (pdmDac && format.slots == 2 && requested.dout2 < 0)) {
        ESP_LOGE(TAG, "Invalid initial I2S pins");
        return false;
    }
    if (!formatImplemented(format)) {
        ESP_LOGE(TAG, "Requested I2S format is not implemented");
        return false;
    }
    if ((spdifBmc || pdmDac) && first != Direction::Tx) {
        ESP_LOGE(TAG, "SPDIF BMC and PDM DAC formats are TX-only");
        return false;
    }
    if (pdmDac && controllerOf(&s) != 0) {
        ESP_LOGE(TAG, "PCM-to-PDM output is only supported on I2S0");
        return false;
    }

    s.pins = requested;
    s.pins.dout = first == Direction::Tx ? requested.dout : -1;
    s.pins.din = first == Direction::Rx ? requested.din : -1;
    s.format = format;
    s.physicalMaster = first;

    i2s_chan_config_t cfg = {};
    // Explicit controller IDs make independent ports addressable. Casting also
    // allows this source to compile when a target names fewer enum values.
    cfg.id = static_cast<i2s_port_t>(controllerOf(&s));
    cfg.role = I2S_ROLE_MASTER;
    cfg.dma_desc_num = DMA_DESC_NUM;
    cfg.dma_frame_num = static_cast<uint16_t>(DMA_FRAME_NUM * format.rateMultiplier);
    cfg.auto_clear_after_cb = true;
    cfg.allow_pd = false;
    cfg.intr_priority = 2;

    if (pdmDac) {
#if defined(SOC_I2S_SUPPORTS_PCM2PDM) && SOC_I2S_SUPPORTS_PCM2PDM && \
    defined(SOC_I2S_HW_VERSION_2) && SOC_I2S_HW_VERSION_2
        // Direct-to-filter DAC mode: PCM is modulated by I2S hardware.
        // Controller 0 is allocated TX-only because PDM mode is not compatible
        // with this manager's standard-mode full-duplex channel pair.
        esp_err_t err = i2s_new_channel(&cfg, &s.txHandle, nullptr);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "I2S0 PDM allocation failed: %s", esp_err_to_name(err));
            resetPortState(s);
            return false;
        }

        i2s_pdm_tx_config_t pdmCfg = {};
        pdmCfg.clk_cfg = makePdmDacClockConfig(rate);
        pdmCfg.slot_cfg = I2S_PDM_TX_SLOT_PCM_FMT_DAC_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_16BIT,
            format.slots == 1 ? I2S_SLOT_MODE_MONO : I2S_SLOT_MODE_STEREO);
        pdmCfg.gpio_cfg.clk = I2S_GPIO_UNUSED;
        pdmCfg.gpio_cfg.dout = static_cast<gpio_num_t>(requested.dout);
#if defined(SOC_I2S_PDM_MAX_TX_LINES) && SOC_I2S_PDM_MAX_TX_LINES > 1
        pdmCfg.gpio_cfg.dout2 = format.slots == 2
                                    ? static_cast<gpio_num_t>(requested.dout2)
                                    : I2S_GPIO_UNUSED;
#else
        if (format.slots == 2) {
            ESP_LOGE(TAG, "This target does not support two-line PDM DAC output");
            i2s_del_channel(s.txHandle);
            resetPortState(s);
            return false;
        }
#endif
        pdmCfg.gpio_cfg.invert_flags.clk_inv = false;

        err = i2s_channel_init_pdm_tx_mode(s.txHandle, &pdmCfg);
        if (err == ESP_OK) {
            applyPrecisePdmMclkIfNeeded(pdmCfg.clk_cfg);
            i2s_event_callbacks_t cb = {};
            cb.on_sent = &dispatchTx;
            err = i2s_channel_register_event_callback(s.txHandle, &cb, &s);
        }
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "I2S0 PDM setup failed: %s", esp_err_to_name(err));
            i2s_del_channel(s.txHandle);
            resetPortState(s);
            return false;
        }

        s.sampleRate = rate;
        s.pairCreated = true;
        return true;
#else
        ESP_LOGE(TAG, "Direct PCM-to-PDM DAC mode is not supported by this target/IDF");
        resetPortState(s);
        return false;
#endif
    }

    esp_err_t err = i2s_new_channel(&cfg, &s.txHandle, &s.rxHandle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2S%u allocation failed: %s", controllerOf(&s), esp_err_to_name(err));
        resetPortState(s);
        return false;
    }

    const i2s_std_config_t txCfg = makeStdConfig(s.pins, rate, true, first == Direction::Tx, format);
    const i2s_std_config_t rxCfg = makeStdConfig(s.pins, rate, false, first == Direction::Rx, format);
    if (first == Direction::Tx) {
        err = i2s_channel_init_std_mode(s.txHandle, &txCfg);
        if (err == ESP_OK) err = i2s_channel_init_std_mode(s.rxHandle, &rxCfg);
    } else {
        err = i2s_channel_init_std_mode(s.rxHandle, &rxCfg);
        if (err == ESP_OK) err = i2s_channel_init_std_mode(s.txHandle, &txCfg);
    }

    if (err == ESP_OK) {
        i2s_event_callbacks_t cb = {};
        cb.on_sent = &dispatchTx;
        err = i2s_channel_register_event_callback(s.txHandle, &cb, &s);
    }
    if (err == ESP_OK) {
        i2s_event_callbacks_t cb = {};
        cb.on_recv = &dispatchRx;
        err = i2s_channel_register_event_callback(s.rxHandle, &cb, &s);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2S%u setup failed: %s", controllerOf(&s), esp_err_to_name(err));
        if (s.txHandle) i2s_del_channel(s.txHandle);
        if (s.rxHandle) i2s_del_channel(s.rxHandle);
        resetPortState(s);
        return false;
    }

    s.sampleRate = rate;
    s.pairCreated = true;
    return true;
}

OSIZE bool acquire(Direction d, const Pins &pins, i2s_chan_handle_t *out, uint32_t rate, const Format &format) {
    PortState *s = stateFor(pins.controller);
    if (!s || !out || d == Direction::None) return false;
    *out = nullptr;

    if ((d == Direction::Tx && s->txAcquired) || (d == Direction::Rx && s->rxAcquired)) {
        ESP_LOGE(TAG, "I2S%u direction already acquired", pins.controller);
        return false;
    }
    if (!s->pairCreated) {
        if (!createPair(*s, pins, d, rate, format)) return false;
    } else {
        if (s->sampleRate != rate || !sameFormat(s->format, format)) {
            ESP_LOGE(TAG, "I2S%u rate/format conflicts with its existing channel pair", pins.controller);
            return false;
        }
        if (pins.bclk != s->pins.bclk || pins.ws != s->pins.ws || !sameOptionalPin(pins.mclk, s->pins.mclk)) {
            ESP_LOGE(TAG, "I2S%u TX/RX clock pins must match", pins.controller);
            return false;
        }

        const bool tx = d == Direction::Tx;
        const int8_t dataPin = tx ? pins.dout : pins.din;
        if (dataPin < 0) return false;
        Pins next = s->pins;
        int8_t &configuredPin = tx ? next.dout : next.din;
        if (configuredPin != dataPin) {
            if (tx ? s->txEnabled : s->rxEnabled) {
                ESP_LOGE(TAG, "Cannot change enabled I2S%u data pin", pins.controller);
                return false;
            }
            configuredPin = dataPin;
            const i2s_chan_handle_t h = tx ? s->txHandle : s->rxHandle;
            const auto gpio = makeGpioConfig(next, tx, d == s->physicalMaster);
            const esp_err_t err = i2s_channel_reconfig_std_gpio(h, &gpio);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "I2S%u GPIO setup failed: %s", pins.controller, esp_err_to_name(err));
                return false;
            }
            s->pins = next;
        }
    }

    if (d == Direction::Tx) {
        s->txAcquired = true;
        *out = s->txHandle;
    } else {
        s->rxAcquired = true;
        *out = s->rxHandle;
    }
    return true;
}

bool anyActive() {
    for (const PortState &s : ports)
        if (s.txActive || s.rxActive) return true;
    return false;
}

int8_t findActiveOwner() {
    for (uint8_t i = 0; i < MAX_PORTS; ++i) {
        if (ports[i].txActive) return ownerCode(i, Direction::Tx);
        if (ports[i].rxActive) return ownerCode(i, Direction::Rx);
    }
    return -1;
}

bool enabled(const PortState &s, Direction d) { return d == Direction::Tx ? s.txEnabled : s.rxEnabled; }

OSIZE
bool enableChannel(PortState &s, Direction d) {
    i2s_chan_handle_t h = d == Direction::Tx ? s.txHandle : s.rxHandle;
    if (!h) return false;
    if (enabled(s, d)) return true;
    const esp_err_t err = i2s_channel_enable(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2S%u %s enable failed: %s", controllerOf(&s), d == Direction::Tx ? "TX" : "RX",
                 esp_err_to_name(err));
        return false;
    }
    if (d == Direction::Tx)
        s.txEnabled = true;
    else
        s.rxEnabled = true;
    return true;
}

OSIZE
bool disableChannel(PortState &s, Direction d) {
    bool &isEnabled = d == Direction::Tx ? s.txEnabled : s.rxEnabled;
    if (!isEnabled) return true;
    const i2s_chan_handle_t h = d == Direction::Tx ? s.txHandle : s.rxHandle;
    if (!h) {
        isEnabled = false;
        return true;
    }
    const esp_err_t err = i2s_channel_disable(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2S%u %s disable failed: %s", controllerOf(&s), d == Direction::Tx ? "TX" : "RX",
                 esp_err_to_name(err));
        return false;
    }
    isEnabled = false;
    return true;
}

OSIZE
bool restoreEnabled(PortState &s, bool oldTx, bool oldRx) {
    const Direction master = s.physicalMaster;
    const Direction slave = master == Direction::Tx ? Direction::Rx : Direction::Tx;
    if ((master == Direction::Tx ? oldTx : oldRx) && !enableChannel(s, master)) return false;
    if ((slave == Direction::Tx ? oldTx : oldRx) && !enableChannel(s, slave)) return false;
    return true;
}

OSIZE
void clearHandler(PortState &s, Direction d, void *context) {
    if (d == Direction::None || !context) return;
    portENTER_CRITICAL(&s.callbackMux);
    EventCallback &cb = d == Direction::Tx ? s.txCallback : s.rxCallback;
    void *&registered = d == Direction::Tx ? s.txContext : s.rxContext;
    if (registered == context) {
        cb = nullptr;
        registered = nullptr;
    }
    portEXIT_CRITICAL(&s.callbackMux);

    for (;;) {
        portENTER_CRITICAL(&s.callbackMux);
        const bool busy = d == Direction::Tx ? s.txCallbacksInFlight != 0 : s.rxCallbacksInFlight != 0;
        portEXIT_CRITICAL(&s.callbackMux);
        if (!busy) break;
        vTaskDelay(1);
    }
}

} // namespace

bool AudioI2SPort::acquireTx(const Pins &pins, i2s_chan_handle_t *handle) { return acquireTx(pins, handle, Format{}); }
bool AudioI2SPort::acquireTx(const Pins &pins, i2s_chan_handle_t *handle, const Format &format) {
    return acquire(Direction::Tx, pins, handle, static_cast<uint32_t>(std::lround(AudioStream::sampleRate())), format);
}
bool AudioI2SPort::acquireRx(const Pins &pins, i2s_chan_handle_t *handle) { return acquireRx(pins, handle, Format{}); }
bool AudioI2SPort::acquireRx(const Pins &pins, i2s_chan_handle_t *handle, const Format &format) {
    return acquire(Direction::Rx, pins, handle, static_cast<uint32_t>(std::lround(AudioStream::sampleRate())), format);
}

OSIZE
bool AudioI2SPort::setEventHandler(uint8_t controller, Direction d, EventCallback callback, void *context) {
    PortState *s = stateFor(controller);
    if (!s || !s->pairCreated || d == Direction::None || !callback || !context) return false;
    if ((d == Direction::Tx && !s->txAcquired) || (d == Direction::Rx && !s->rxAcquired)) return false;

    portENTER_CRITICAL(&s->callbackMux);
    EventCallback &current = d == Direction::Tx ? s->txCallback : s->rxCallback;
    void *&currentContext = d == Direction::Tx ? s->txContext : s->rxContext;
    const bool available = current == nullptr || currentContext == context;
    if (available) {
        current = callback;
        currentContext = context;
    }
    portEXIT_CRITICAL(&s->callbackMux);
    return available;
}

void AudioI2SPort::clearEventHandler(uint8_t controller, Direction d, void *context) {
    if (PortState *s = stateFor(controller)) clearHandler(*s, d, context);
}

OSIZE
bool AudioI2SPort::start(uint8_t controller, Direction d) {
    PortState *s = stateFor(controller);
    if (!s || !s->pairCreated || d == Direction::None) return false;
    if ((d == Direction::Tx && (!s->txAcquired || !s->txHandle)) ||
        (d == Direction::Rx && (!s->rxAcquired || !s->rxHandle)))
        return false;
    if (d == Direction::Tx ? s->txActive : s->rxActive) return true;

    const bool firstActive = !anyActive();
    if (firstActive && !AudioStream::setExternalUpdateClock(true)) {
        ESP_LOGE(TAG, "Could not enable external AudioStream clock");
        return false;
    }

    const Direction master = s->physicalMaster;
    if (d != master && !enabled(*s, master) && !enableChannel(*s, master)) {
        if (firstActive) AudioStream::setExternalUpdateClock(false);
        return false;
    }
    if (!enableChannel(*s, d)) {
        if (!s->txActive && !s->rxActive) {
            const Direction slave = master == Direction::Tx ? Direction::Rx : Direction::Tx;
            disableChannel(*s, slave);
            disableChannel(*s, master);
        }
        if (firstActive) AudioStream::setExternalUpdateClock(false);
        return false;
    }

    if (d == Direction::Tx)
        s->txActive = true;
    else
        s->rxActive = true;
    if (firstActive || getSchedulerOwner() < 0) setSchedulerOwner(ownerCode(controller, d));
    return true;
}

OSIZE
void AudioI2SPort::stop(uint8_t controller, Direction d) {
    PortState *s = stateFor(controller);
    if (!s || d == Direction::None) return;
    const bool wasActive = d == Direction::Tx ? s->txActive : s->rxActive;
    if (!wasActive) return;
    if (d == Direction::Tx)
        s->txActive = false;
    else
        s->rxActive = false;

    if (d != s->physicalMaster) disableChannel(*s, d);
    if (!s->txActive && !s->rxActive) {
        const Direction slave = s->physicalMaster == Direction::Tx ? Direction::Rx : Direction::Tx;
        disableChannel(*s, slave);
        disableChannel(*s, s->physicalMaster);
    }

    if (getSchedulerOwner() == ownerCode(controller, d)) {
        setSchedulerOwner(findActiveOwner());
    }
    if (!anyActive()) {
        setSchedulerOwner(-1);
        // The update clock source is fixed after AudioMemory(). Stopping the
        // last I2S clock stops audio updates; do not claim to restore a software
        // clock that was deliberately not selected during initialization.
    }
}

OSIZE
void AudioI2SPort::release(uint8_t controller, Direction d) {
    PortState *s = stateFor(controller);
    if (!s || d == Direction::None) return;
    if (d == Direction::Tx)
        s->txAcquired = false;
    else
        s->rxAcquired = false;
    if (s->txAcquired || s->rxAcquired || s->txActive || s->rxActive || s->txEnabled || s->rxEnabled) return;

    clearHandler(*s, Direction::Tx, s->txContext);
    clearHandler(*s, Direction::Rx, s->rxContext);
    if (s->txHandle) i2s_del_channel(s->txHandle);
    if (s->rxHandle) i2s_del_channel(s->rxHandle);
    resetPortState(*s);
}

OSIZE
bool AudioI2SPort::setSampleRate(float rate) {
    if (!std::isfinite(rate) || rate <= 0.0f) return false;
    AudioGraphUpdateLock graphLock;
    const long rounded = std::lround(rate);
    if (rounded <= 0) return false;
    const uint32_t newRate = static_cast<uint32_t>(rounded);
    const float oldRateFloat = AudioStream::sampleRate();
    const uint32_t oldRate = static_cast<uint32_t>(std::lround(oldRateFloat));
    if (newRate == oldRate) return AudioStream::setSampleRate(rate);

    bool oldTx[MAX_PORTS]{}, oldRx[MAX_PORTS]{};
    for (uint8_t i = 0; i < MAX_PORTS; ++i) {
        oldTx[i] = ports[i].txEnabled;
        oldRx[i] = ports[i].rxEnabled;
    }

    for (PortState &s : ports) {
        if (!s.pairCreated) continue;
        const Direction slave = s.physicalMaster == Direction::Tx ? Direction::Rx : Direction::Tx;
        if (!disableChannel(s, slave) || !disableChannel(s, s.physicalMaster)) {
            for (uint8_t i = 0; i < MAX_PORTS; ++i) {
                if (ports[i].pairCreated) restoreEnabled(ports[i], oldTx[i], oldRx[i]);
            }
            return false;
        }
    }

    for (;;) {
        bool busy = false;
        for (PortState &s : ports) {
            portENTER_CRITICAL(&s.callbackMux);
            busy |= s.txCallbacksInFlight != 0 || s.rxCallbacksInFlight != 0;
            portEXIT_CRITICAL(&s.callbackMux);
        }
        if (!busy) break;
        vTaskDelay(1);
    }

    auto clockConfig = [](uint32_t hz, const Format &format) {
        i2s_std_clk_config_t cfg = I2S_STD_CLK_DEFAULT_CONFIG(hz * format.rateMultiplier);
        cfg.clk_src = I2S_CLK_SRC_DEFAULT;
        cfg.mclk_multiple = format.encoding == AudioI2SPort::DataEncoding::SpdifBMC
                                ? I2S_MCLK_MULTIPLE_128
                                : I2S_MCLK_MULTIPLE_256;
        return cfg;
    };

    auto reconfigurePortClock = [&](PortState &s, uint32_t hz) -> esp_err_t {
        if (s.format.mode == AudioI2SPort::PeripheralMode::PDM) {
#if defined(SOC_I2S_SUPPORTS_PCM2PDM) && SOC_I2S_SUPPORTS_PCM2PDM && \
    defined(SOC_I2S_HW_VERSION_2) && SOC_I2S_HW_VERSION_2
            i2s_pdm_tx_clk_config_t cfg = makePdmDacClockConfig(hz);
            const esp_err_t err = i2s_channel_reconfig_pdm_tx_clock(s.txHandle, &cfg);
            if (err == ESP_OK) applyPrecisePdmMclkIfNeeded(cfg);
            return err;
#else
            (void)hz;
            return ESP_ERR_NOT_SUPPORTED;
#endif
        }

        const Direction slave = s.physicalMaster == Direction::Tx ? Direction::Rx : Direction::Tx;
        const i2s_chan_handle_t master = s.physicalMaster == Direction::Tx ? s.txHandle : s.rxHandle;
        const i2s_chan_handle_t slaveHandle = slave == Direction::Tx ? s.txHandle : s.rxHandle;
        const auto cfg = clockConfig(hz, s.format);
        esp_err_t err = i2s_channel_reconfig_std_clock(master, &cfg);
        if (err == ESP_OK) err = i2s_channel_reconfig_std_clock(slaveHandle, &cfg);
        return err;
    };

    esp_err_t err = ESP_OK;
    for (PortState &s : ports) {
        if (!s.pairCreated) continue;
        err = reconfigurePortClock(s, newRate);
        if (err != ESP_OK) break;
    }
    if (err == ESP_OK && !AudioStream::setSampleRate(rate)) err = ESP_FAIL;

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2S sample-rate reconfiguration failed: %s", esp_err_to_name(err));
        for (PortState &s : ports) {
            if (!s.pairCreated) continue;
            reconfigurePortClock(s, oldRate);
            s.sampleRate = oldRate;
        }
        AudioStream::setSampleRate(oldRateFloat);
        for (uint8_t i = 0; i < MAX_PORTS; ++i) {
            if (ports[i].pairCreated) restoreEnabled(ports[i], oldTx[i], oldRx[i]);
        }
        return false;
    }

    for (PortState &s : ports)
        if (s.pairCreated) s.sampleRate = newRate;
    for (uint8_t i = 0; i < MAX_PORTS; ++i) {
        if (ports[i].pairCreated && !restoreEnabled(ports[i], oldTx[i], oldRx[i])) {
            ESP_LOGE(TAG, "I2S%u sample rate changed but channel re-enable failed", i);
            return false;
        }
    }
    return true;
}

OSIZE
bool AudioI2SPort::isClockOwner(uint8_t controller, Direction d) {
    const int8_t requestedOwner = (d == Direction::None || controller >= MAX_PORTS)
                                      ? -1
                                      : static_cast<int8_t>(controller * 2U + (d == Direction::Rx ? 1U : 0U));
    return schedulerOwner.load(std::memory_order_acquire) == requestedOwner;
}
