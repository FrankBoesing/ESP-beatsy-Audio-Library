#include "i2s_port.h"

#include <cmath>
#include <esp_err.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "AudioStream.h"

namespace {
using Direction = AudioI2SPort::Direction;
using Pins = AudioI2SPort::Pins;
using EventCallback = AudioI2SPort::EventCallback;
constexpr char TAG[] = "AudioI2SPort";
constexpr uint16_t DMA_DESC_NUM = 6;
constexpr uint16_t DMA_FRAME_NUM = AUDIO_BLOCK_SAMPLES;

i2s_chan_handle_t txHandle = nullptr;
i2s_chan_handle_t rxHandle = nullptr;
Pins sharedPins = {-1, -1, -1, -1, -1};
Direction physicalMaster = Direction::None;
volatile Direction clockOwner = Direction::None;
bool pairCreated = false;
uint32_t portSampleRate = 0;
bool txAcquired = false;
bool rxAcquired = false;
bool txActive = false;
bool rxActive = false;
bool txEnabled = false;
bool rxEnabled = false;

portMUX_TYPE callbackMux = portMUX_INITIALIZER_UNLOCKED;
EventCallback txEventCallback = nullptr;
void *txEventContext = nullptr;
EventCallback rxEventCallback = nullptr;
void *rxEventContext = nullptr;
volatile uint32_t txCallbacksInFlight = 0;
volatile uint32_t rxCallbacksInFlight = 0;

static bool sameClockPin(int8_t requested, int8_t configured) {
    return requested == configured;
}
static bool sameOptionalPin(int8_t requested, int8_t configured) {
    return requested < 0 || requested == configured;
}

static i2s_std_config_t makeStdConfig(const Pins &pins, uint32_t rate,
                                      bool transmit, bool master) {
    i2s_std_config_t cfg = {};
    cfg.clk_cfg.sample_rate_hz = rate;
    cfg.clk_cfg.clk_src = I2S_CLK_SRC_DEFAULT;
    cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    cfg.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
        I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);

    cfg.gpio_cfg.mclk = (master && pins.mclk >= 0)
                            ? static_cast<gpio_num_t>(pins.mclk) : I2S_GPIO_UNUSED;
    cfg.gpio_cfg.bclk = pins.bclk >= 0 ? static_cast<gpio_num_t>(pins.bclk) : I2S_GPIO_UNUSED;
    cfg.gpio_cfg.ws = pins.ws >= 0 ? static_cast<gpio_num_t>(pins.ws) : I2S_GPIO_UNUSED;
    cfg.gpio_cfg.dout = (transmit && pins.dout >= 0)
                            ? static_cast<gpio_num_t>(pins.dout) : I2S_GPIO_UNUSED;
    cfg.gpio_cfg.din = (!transmit && pins.din >= 0)
                           ? static_cast<gpio_num_t>(pins.din) : I2S_GPIO_UNUSED;
    cfg.gpio_cfg.invert_flags.mclk_inv = false;
    cfg.gpio_cfg.invert_flags.bclk_inv = false;
    cfg.gpio_cfg.invert_flags.ws_inv = false;
    return cfg;
}

static i2s_std_gpio_config_t makeGpioConfig(const Pins &pins,
                                            bool transmit, bool master) {
    i2s_std_gpio_config_t cfg = {};
    cfg.mclk = (master && pins.mclk >= 0)
                   ? static_cast<gpio_num_t>(pins.mclk) : I2S_GPIO_UNUSED;
    cfg.bclk = pins.bclk >= 0 ? static_cast<gpio_num_t>(pins.bclk) : I2S_GPIO_UNUSED;
    cfg.ws = pins.ws >= 0 ? static_cast<gpio_num_t>(pins.ws) : I2S_GPIO_UNUSED;
    cfg.dout = (transmit && pins.dout >= 0)
                   ? static_cast<gpio_num_t>(pins.dout) : I2S_GPIO_UNUSED;
    cfg.din = (!transmit && pins.din >= 0)
                  ? static_cast<gpio_num_t>(pins.din) : I2S_GPIO_UNUSED;
    cfg.invert_flags.mclk_inv = false;
    cfg.invert_flags.bclk_inv = false;
    cfg.invert_flags.ws_inv = false;
    return cfg;
}

static bool IRAM_ATTR dispatchTx(i2s_chan_handle_t channel, i2s_event_data_t *event, void *) {
    EventCallback callback;
    void *context;
    portENTER_CRITICAL_ISR(&callbackMux);
    ++txCallbacksInFlight;
    callback = txEventCallback;
    context = txEventContext;
    portEXIT_CRITICAL_ISR(&callbackMux);

    const bool taskWoken = callback != nullptr ? callback(channel, event, context) : false;

    portENTER_CRITICAL_ISR(&callbackMux);
    --txCallbacksInFlight;
    portEXIT_CRITICAL_ISR(&callbackMux);
    return taskWoken;
}

static bool IRAM_ATTR dispatchRx(i2s_chan_handle_t channel, i2s_event_data_t *event, void *) {
    EventCallback callback;
    void *context;
    portENTER_CRITICAL_ISR(&callbackMux);
    ++rxCallbacksInFlight;
    callback = rxEventCallback;
    context = rxEventContext;
    portEXIT_CRITICAL_ISR(&callbackMux);

    const bool taskWoken = callback != nullptr ? callback(channel, event, context) : false;

    portENTER_CRITICAL_ISR(&callbackMux);
    --rxCallbacksInFlight;
    portEXIT_CRITICAL_ISR(&callbackMux);
    return taskWoken;
}

static void resetPortState() {
    txHandle = nullptr;
    rxHandle = nullptr;
    sharedPins = {-1, -1, -1, -1, -1};
    physicalMaster = Direction::None;
    clockOwner = Direction::None;
    pairCreated = false;
    portSampleRate = 0;
    txAcquired = rxAcquired = false;
    txActive = rxActive = false;
    txEnabled = rxEnabled = false;
}

static bool createPair(const Pins &requested, Direction first, uint32_t rate) {
    if (requested.bclk < 0 || requested.ws < 0 ||
        (first == Direction::Tx && requested.dout < 0) ||
        (first == Direction::Rx && requested.din < 0)) {
        ESP_LOGE(TAG, "Invalid initial I2S pins");
        return false;
    }

    sharedPins = {requested.bclk, requested.ws, -1, -1, requested.mclk};
    if (first == Direction::Tx) sharedPins.dout = requested.dout;
    else sharedPins.din = requested.din;
    physicalMaster = first;

    i2s_chan_config_t chanCfg = {};
    chanCfg.id = I2S_NUM_AUTO;
    chanCfg.role = I2S_ROLE_MASTER;
    chanCfg.dma_desc_num = DMA_DESC_NUM;
    chanCfg.dma_frame_num = DMA_FRAME_NUM;
    chanCfg.auto_clear_after_cb = true;
    chanCfg.allow_pd = false;
    chanCfg.intr_priority = 2;

    esp_err_t err = i2s_new_channel(&chanCfg, &txHandle, &rxHandle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel(TX+RX) failed: %s", esp_err_to_name(err));
        resetPortState();
        return false;
    }

    const i2s_std_config_t txCfg = makeStdConfig(sharedPins, rate, true, first == Direction::Tx);
    const i2s_std_config_t rxCfg = makeStdConfig(sharedPins, rate, false, first == Direction::Rx);

    // Per ESP-IDF's paired-channel model, initialize the intended master first.
    if (first == Direction::Tx) {
        err = i2s_channel_init_std_mode(txHandle, &txCfg);
        if (err == ESP_OK) err = i2s_channel_init_std_mode(rxHandle, &rxCfg);
    } else {
        err = i2s_channel_init_std_mode(rxHandle, &rxCfg);
        if (err == ESP_OK) err = i2s_channel_init_std_mode(txHandle, &txCfg);
    }

    if (err == ESP_OK) {
        i2s_event_callbacks_t cb = {};
        cb.on_sent = &dispatchTx;
        err = i2s_channel_register_event_callback(txHandle, &cb, nullptr);
    }
    if (err == ESP_OK) {
        i2s_event_callbacks_t cb = {};
        cb.on_recv = &dispatchRx;
        err = i2s_channel_register_event_callback(rxHandle, &cb, nullptr);
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2S full-duplex channel initialization failed: %s", esp_err_to_name(err));
        if (txHandle != nullptr) i2s_del_channel(txHandle);
        if (rxHandle != nullptr) i2s_del_channel(rxHandle);
        resetPortState();
        return false;
    }

    portSampleRate = rate;
    pairCreated = true;
    return true;
}

static bool acquire(Direction direction, const Pins &requested,
                    i2s_chan_handle_t *outHandle, uint32_t rate) {
    if (outHandle == nullptr) return false;
    *outHandle = nullptr;

    if ((direction == Direction::Tx && txAcquired) ||
        (direction == Direction::Rx && rxAcquired) || direction == Direction::None) {
        ESP_LOGE(TAG, "I2S direction already acquired or invalid");
        return false;
    }

    if (!pairCreated) {
        if (!createPair(requested, direction, rate)) return false;
    } else {
        if (rate != portSampleRate) {
            ESP_LOGE(TAG, "I2S sample rate mismatch (%lu requested, %lu active)",
                     static_cast<unsigned long>(rate), static_cast<unsigned long>(portSampleRate));
            return false;
        }
        if (!sameClockPin(requested.bclk, sharedPins.bclk) ||
            !sameClockPin(requested.ws, sharedPins.ws) ||
            !sameOptionalPin(requested.mclk, sharedPins.mclk)) {
            ESP_LOGE(TAG, "TX/RX BCLK, WS and MCLK pins must match");
            return false;
        }

        Pins next = sharedPins;
        const bool isTx = direction == Direction::Tx;
        const int8_t requestedDataPin = isTx ? requested.dout : requested.din;
        if (requestedDataPin < 0) return false;

        const int8_t currentDataPin = isTx ? next.dout : next.din;
        const bool dataPinChanged = currentDataPin != requestedDataPin;
        const bool directionEnabled = isTx ? txEnabled : rxEnabled;
        if (dataPinChanged && directionEnabled) {
            ESP_LOGE(TAG, "Cannot change I2S data pin while its channel is enabled");
            return false;
        }

        if (isTx) next.dout = requestedDataPin;
        else next.din = requestedDataPin;

        if (dataPinChanged) {
            const i2s_chan_handle_t h = isTx ? txHandle : rxHandle;
            const i2s_std_gpio_config_t cfg =
                makeGpioConfig(next, isTx, direction == physicalMaster);
            const esp_err_t err = i2s_channel_reconfig_std_gpio(h, &cfg);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "I2S GPIO reconfiguration failed: %s", esp_err_to_name(err));
                return false;
            }
            sharedPins = next;
        }
    }

    if (direction == Direction::Tx) {
        txAcquired = true;
        *outHandle = txHandle;
    } else {
        rxAcquired = true;
        *outHandle = rxHandle;
    }
    return true;
}

static bool setHandler(Direction d, EventCallback cb, void *ctx) {
    if (!pairCreated || cb == nullptr || ctx == nullptr) return false;
    if ((d == Direction::Tx && !txAcquired) ||
        (d == Direction::Rx && !rxAcquired) || d == Direction::None) return false;

    portENTER_CRITICAL(&callbackMux);
    EventCallback &current = d == Direction::Tx ? txEventCallback : rxEventCallback;
    void *&currentContext = d == Direction::Tx ? txEventContext : rxEventContext;
    const bool free = current == nullptr || currentContext == ctx;
    if (free) {
        current = cb;
        currentContext = ctx;
    }
    portEXIT_CRITICAL(&callbackMux);
    return free;
}

static void clearHandler(Direction d, void *ctx) {
    if (d == Direction::None || ctx == nullptr) return;

    portENTER_CRITICAL(&callbackMux);
    EventCallback &current = d == Direction::Tx ? txEventCallback : rxEventCallback;
    void *&currentContext = d == Direction::Tx ? txEventContext : rxEventContext;
    volatile uint32_t &inFlight = d == Direction::Tx ? txCallbacksInFlight : rxCallbacksInFlight;
    if (currentContext == ctx) {
        current = nullptr;
        currentContext = nullptr;
    }
    portEXIT_CRITICAL(&callbackMux);

    // Do not return while an ISR still holds a pointer to the owning object.
    for (;;) {
        portENTER_CRITICAL(&callbackMux);
        const bool busy = inFlight != 0;
        portEXIT_CRITICAL(&callbackMux);
        if (!busy) break;
        vTaskDelay(1);
    }
}

static bool enableChannel(Direction d) {
    i2s_chan_handle_t h = d == Direction::Tx ? txHandle : rxHandle;
    if (h == nullptr) return false;
    const esp_err_t err = i2s_channel_enable(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2S %s enable failed: %s",
                 d == Direction::Tx ? "TX" : "RX", esp_err_to_name(err));
        return false;
    }
    if (d == Direction::Tx) txEnabled = true;
    else rxEnabled = true;
    return true;
}

static bool disableChannel(Direction d) {
    bool &enabled = d == Direction::Tx ? txEnabled : rxEnabled;
    if (!enabled) return true;
    i2s_chan_handle_t h = d == Direction::Tx ? txHandle : rxHandle;
    const esp_err_t err = i2s_channel_disable(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2S %s disable failed: %s",
                 d == Direction::Tx ? "TX" : "RX", esp_err_to_name(err));
        return false;
    }
    enabled = false;
    return true;
}

static bool restoreEnabled(bool oldTx, bool oldRx) {
    const Direction master = physicalMaster;
    const Direction slave = master == Direction::Tx ? Direction::Rx : Direction::Tx;
    const bool oldMaster = master == Direction::Tx ? oldTx : oldRx;
    const bool oldSlave = slave == Direction::Tx ? oldTx : oldRx;
    if (oldSlave && !(slave == Direction::Tx ? txEnabled : rxEnabled) && !enableChannel(slave)) return false;
    if (oldMaster && !(master == Direction::Tx ? txEnabled : rxEnabled) && !enableChannel(master)) return false;
    return true;
}

} // namespace

bool AudioI2SPort::acquireTx(const Pins &pins, i2s_chan_handle_t *handle) {
    const uint32_t rate = static_cast<uint32_t>(std::lround(AudioStream::sampleRate()));
    return acquire(Direction::Tx, pins, handle, rate);
}

bool AudioI2SPort::acquireRx(const Pins &pins, i2s_chan_handle_t *handle) {
    const uint32_t rate = static_cast<uint32_t>(std::lround(AudioStream::sampleRate()));
    return acquire(Direction::Rx, pins, handle, rate);
}

bool AudioI2SPort::setEventHandler(Direction direction, EventCallback callback, void *context) {
    return setHandler(direction, callback, context);
}

void AudioI2SPort::clearEventHandler(Direction direction, void *context) {
    clearHandler(direction, context);
}

bool AudioI2SPort::start(Direction direction) {
    if (!pairCreated || direction == Direction::None) return false;
    if ((direction == Direction::Tx && (!txAcquired || txHandle == nullptr)) ||
        (direction == Direction::Rx && (!rxAcquired || rxHandle == nullptr))) return false;
    if ((direction == Direction::Tx && txActive) || (direction == Direction::Rx && rxActive)) return true;

    const bool noActive = !txActive && !rxActive;
    if (noActive && !AudioStream::setExternalUpdateClock(true)) {
        ESP_LOGE(TAG, "Could not enable the external audio clock");
        return false;
    }

    const Direction slave = physicalMaster == Direction::Tx ? Direction::Rx : Direction::Tx;
    if (noActive && direction != physicalMaster) {
        const bool masterEnabled = physicalMaster == Direction::Tx ? txEnabled : rxEnabled;
        if (!masterEnabled && !enableChannel(physicalMaster)) {
            AudioStream::setExternalUpdateClock(false);
            return false;
        }
    }

    const bool alreadyEnabled = direction == Direction::Tx ? txEnabled : rxEnabled;
    if (!alreadyEnabled && !enableChannel(direction)) {
        if (noActive) {
            disableChannel(slave);
            disableChannel(physicalMaster);
            AudioStream::setExternalUpdateClock(false);
        }
        return false;
    }

    if (direction == Direction::Tx) txActive = true;
    else rxActive = true;
    if (clockOwner == Direction::None || noActive) clockOwner = direction;
    return true;
}

void AudioI2SPort::stop(Direction direction) {
    if (direction == Direction::None) return;
    const bool wasActive = direction == Direction::Tx ? txActive : rxActive;
    if (!wasActive) return;

    if (direction == Direction::Tx) txActive = false;
    else rxActive = false;

    const bool otherActive = direction == Direction::Tx ? rxActive : txActive;
    if (direction != physicalMaster) disableChannel(direction);

    if (clockOwner == direction) {
        clockOwner = otherActive
            ? (direction == Direction::Tx ? Direction::Rx : Direction::Tx)
            : Direction::None;
    }

    if (!txActive && !rxActive) {
        const Direction slave = physicalMaster == Direction::Tx ? Direction::Rx : Direction::Tx;
        disableChannel(slave);
        disableChannel(physicalMaster);
        clockOwner = Direction::None;
        if (!AudioStream::setExternalUpdateClock(false)) {
            ESP_LOGE(TAG, "Could not restore the software audio clock");
        }
    }
}

void AudioI2SPort::release(Direction direction) {
    if (direction == Direction::Tx) txAcquired = false;
    else if (direction == Direction::Rx) rxAcquired = false;
    else return;

    if (txAcquired || rxAcquired || txActive || rxActive || txEnabled || rxEnabled) return;

    if (txHandle != nullptr) i2s_del_channel(txHandle);
    if (rxHandle != nullptr) i2s_del_channel(rxHandle);

    portENTER_CRITICAL(&callbackMux);
    txEventCallback = nullptr;
    txEventContext = nullptr;
    rxEventCallback = nullptr;
    rxEventContext = nullptr;
    txCallbacksInFlight = 0;
    rxCallbacksInFlight = 0;
    portEXIT_CRITICAL(&callbackMux);

    resetPortState();
}

bool AudioI2SPort::setSampleRate(float rate) {
    if (!std::isfinite(rate) || rate <= 0.0f) return false;
    const long rounded = std::lround(rate);
    if (rounded <= 0) return false;
    const uint32_t newRate = static_cast<uint32_t>(rounded);

    if (!pairCreated) return AudioStream::setSampleRate(rate);
    if (newRate == portSampleRate) return AudioStream::setSampleRate(rate);

    const float oldRateFloat = AudioStream::sampleRate();
    const uint32_t oldRate = portSampleRate;
    const bool oldTxEnabled = txEnabled;
    const bool oldRxEnabled = rxEnabled;
    const Direction slave = physicalMaster == Direction::Tx ? Direction::Rx : Direction::Tx;
    const i2s_chan_handle_t masterHandle = physicalMaster == Direction::Tx ? txHandle : rxHandle;
    const i2s_chan_handle_t slaveHandle = slave == Direction::Tx ? txHandle : rxHandle;

    // Pause both halves of the full-duplex port. Reconfigure both clock
    // records so that TX/RX still have matching frame timing after the change.
    if (!disableChannel(slave) || !disableChannel(physicalMaster)) {
        restoreEnabled(oldTxEnabled, oldRxEnabled);
        return false;
    }

    for (;;) {
        portENTER_CRITICAL(&callbackMux);
        const bool busy = txCallbacksInFlight != 0 || rxCallbacksInFlight != 0;
        portEXIT_CRITICAL(&callbackMux);
        if (!busy) break;
        vTaskDelay(1);
    }

    auto makeClockConfig = [](uint32_t sampleRate) {
        i2s_std_clk_config_t clk = I2S_STD_CLK_DEFAULT_CONFIG(sampleRate);
        clk.clk_src = I2S_CLK_SRC_DEFAULT;
        clk.mclk_multiple = I2S_MCLK_MULTIPLE_256;
        return clk;
    };

    const i2s_std_clk_config_t newClock = makeClockConfig(newRate);
    esp_err_t err = i2s_channel_reconfig_std_clock(masterHandle, &newClock);
    if (err == ESP_OK) {
        err = i2s_channel_reconfig_std_clock(slaveHandle, &newClock);
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2S sample-rate reconfiguration failed: %s", esp_err_to_name(err));
        const i2s_std_clk_config_t oldClock = makeClockConfig(oldRate);
        i2s_channel_reconfig_std_clock(masterHandle, &oldClock);
        i2s_channel_reconfig_std_clock(slaveHandle, &oldClock);
        restoreEnabled(oldTxEnabled, oldRxEnabled);
        return false;
    }

    if (!AudioStream::setSampleRate(rate)) {
        const i2s_std_clk_config_t oldClock = makeClockConfig(oldRate);
        i2s_channel_reconfig_std_clock(masterHandle, &oldClock);
        i2s_channel_reconfig_std_clock(slaveHandle, &oldClock);
        AudioStream::setSampleRate(oldRateFloat);
        restoreEnabled(oldTxEnabled, oldRxEnabled);
        return false;
    }

    portSampleRate = newRate;
    if (!restoreEnabled(oldTxEnabled, oldRxEnabled)) {
        ESP_LOGE(TAG, "I2S sample rate changed, but a channel could not be re-enabled");
        return false;
    }
    return true;
}

bool IRAM_ATTR AudioI2SPort::isClockOwner(Direction direction) {
    return clockOwner == direction;
}
