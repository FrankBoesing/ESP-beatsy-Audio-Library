#include "control_es8388.h"

#include <cmath>

#pragma GCC optimize("Os")

// DAC main volume registers:
//   0x00 = 0 dB
//   0xC0 = approximately -96 dB
// The scale is therefore inverted.
static uint8_t volumeToRegister(float level) {
    if (level <= 0.0f) {
        return 0xC0;
    }
    if (level >= 1.0f) {
        return 0x00;
    }

    const float attenuation = (1.0f - level) * 192.0f;
    return (uint8_t)(attenuation + 0.5f);
}

/*
AudioControlES8388::AudioControlES8388()
    : pins_{33, 32, 21}, wire_(&Wire), i2cAddress_(ES8388_ADDRESS), initialized_(false) {}
*/
AudioControlES8388::AudioControlES8388(const Pins &pins)
    : pins_(pins), wire_(&Wire), i2cAddress_(ES8388_ADDRESS),
      initialized_(false), adcEnabled_(false) {}

bool AudioControlES8388::writeReg(uint8_t reg, uint8_t value) {
    wire_->beginTransmission(i2cAddress_);
    wire_->write(reg);
    wire_->write(value);
    return wire_->endTransmission() == 0;
}

bool AudioControlES8388::readReg(uint8_t reg, uint8_t &value) {
    wire_->beginTransmission(i2cAddress_);
    wire_->write(reg);

    if (wire_->endTransmission(false) != 0) {
        return false;
    }

    if (wire_->requestFrom((int)(i2cAddress_), 1) != 1) {
        return false;
    }

    value = wire_->read();
    return true;
}

bool AudioControlES8388::isConnected() {
    wire_->beginTransmission(i2cAddress_);
    return wire_->endTransmission() == 0;
}

bool AudioControlES8388::enable() {
    initialized_ = false;
    adcEnabled_ = false;

    // Keep the board's external speaker amplifier disabled during reset/init.
    // The headphone output is inside the codec, so it is muted separately below.
    if (pins_.pa_enable >= 0) {
        pinMode(pins_.pa_enable, OUTPUT);
        digitalWrite(pins_.pa_enable, !pins_.pa_active);
    }

    wire_->begin(pins_.sda, pins_.scl, ES8388_I2C_FREQUENCY);

    if (!isConnected()) {
        return false;
    }

    // Silence the DAC and shut down its output drivers before restarting the
    // codec power state machine. Unlike CONTROL1.SCPReset, this keeps the codec's
    // control registers available for a deterministic reinitialization and avoids
    // clearing the DAC mute bit as a side effect of a register reset.
    if (!writeReg(REG_DACCONTROL3, 0x04) ||
        !setDacVolume(0xC0, 0xC0) ||
        !writeReg(REG_DACPOWER, 0xC0)) {
        return false;
    }

    // Espressif's ES8388 driver uses CHIPPOWER=0xFF to stop/power down the codec
    // and CHIPPOWER=0x00 to start its state machine again.
    if (!writeReg(REG_CHIPPOWER, 0xFF)) {
        return false;
    }
    delay(20);
    if (!writeReg(REG_CHIPPOWER, 0x00)) {
        return false;
    }
    delay(20);

    struct RegisterValue {
        uint8_t reg;
        uint8_t value;
    };

    static constexpr RegisterValue initSequence[] = {
        // Keep DAC muted while configuring the codec.
        {REG_DACCONTROL3, 0x04},

        // Chip control and power.
        {REG_CONTROL2, 0x50},
        {REG_CHIPPOWER, 0x00},

        // Espressif-recommended internal DLL settings.
        {0x35, 0xA0},
        {0x37, 0xD0},
        {0x39, 0xD0},

        // ESP32 is I2S master; ES8388 is slave.
        {REG_MASTERMODE, 0x00},

        // Keep DAC and analog outputs powered down during configuration.
        {REG_DACPOWER, 0xC0},
        {REG_CONTROL1, 0x12},

        // DAC: 16-bit I2S, MCLK/FS = 256.
        {REG_DACCONTROL1, 0x18},
        {REG_DACCONTROL2, 0x02},

        // DAC routing.
        {REG_DACCONTROL16, 0x1B},
        {REG_DACCONTROL17, 0x90},
        {REG_DACCONTROL20, 0x90},
        {REG_DACCONTROL21, 0x80},
        {REG_DACCONTROL23, 0x00},

        // Start at minimum DAC volume.
        {REG_DACCONTROL4, 0xC0},
        {REG_DACCONTROL5, 0xC0},

        // Output volume.
        {REG_DACCONTROL24, 0x1E},
        {REG_DACCONTROL25, 0x1E},
        {REG_DACCONTROL26, 0x1E},
        {REG_DACCONTROL27, 0x1E}
    };

    for (const auto &entry : initSequence) {
        if (!writeReg(entry.reg, entry.value)) {
            return false;
        }
    }

    // Allow the codec's analog stages to settle while muted and with outputs off.
    delay(50);

    // Power up DAC and headphone/line outputs only after routing and attenuation
    // are configured. At this point the DAC is still digitally muted.
    if (!writeReg(REG_DACPOWER, 0x3C)) {
        return false;
    }
    delay(50);

    // Release mute at minimum volume, then ramp to the previous default level.
    if (!volume(0.0f) || !unmute()) {
        return false;
    }
    delay(20);

    if (pins_.pa_enable >= 0) {
        digitalWrite(pins_.pa_enable, pins_.pa_active);
    }

    constexpr int volumeRampSteps = 16;
    constexpr float defaultVolume = 0.7f;
    for (int step = 1; step <= volumeRampSteps; ++step) {
        const float level = defaultVolume * (float)step / (float)volumeRampSteps;
        if (!volume(level)) {
            mute();
            if (pins_.pa_enable >= 0) {
                digitalWrite(pins_.pa_enable, !pins_.pa_active);
            }
            return false;
        }
        delay(10);
    }

    initialized_ = true;
    return true;
}

bool AudioControlES8388::disable() {

    bool ok = true;

    if (initialized_) {
        if (!mute()) return false;
        ok &= writeReg(REG_ADCPOWER, 0xFF);
        ok &= writeReg(REG_DACPOWER, 0xC0);
        ok &= writeReg(REG_CHIPPOWER, 0xFF);
    }

    if (pins_.pa_enable >= 0) {
        pinMode(pins_.pa_enable, OUTPUT);
        digitalWrite(pins_.pa_enable, !pins_.pa_active);
    }

    initialized_ = false;
    adcEnabled_ = false;
    return ok;
}

bool AudioControlES8388::setDacVolume(uint8_t left, uint8_t right) {
    return writeReg(REG_DACCONTROL4, left) && writeReg(REG_DACCONTROL5, right);
}

bool AudioControlES8388::volume(float level) {
    const uint8_t value = volumeToRegister(level);
    return setDacVolume(value, value);
}

bool AudioControlES8388::volume(float left, float right) {
    return setDacVolume(volumeToRegister(left), volumeToRegister(right));
}

bool AudioControlES8388::mute() {
    uint8_t reg = 0;
    if (!readReg(REG_DACCONTROL3, reg)) {
        return false;
    }

    reg |= 0x04;
    return writeReg(REG_DACCONTROL3, reg);
}

bool AudioControlES8388::unmute() {
    uint8_t reg = 0;
    if (!readReg(REG_DACCONTROL3, reg)) {
        return false;
    }

    reg &= (uint8_t)(~0x04);
    return writeReg(REG_DACCONTROL3, reg);
}

bool AudioControlES8388::inputLevel(float level) {
    if (!initialized_ || !adcEnabled_ || !std::isfinite(level)) {
        return false;
    }

    if (level < 0.0f) level = 0.0f;
    else if (level > 1.0f) level = 1.0f;

    // ES8388 ADC PGA gain: register 0x09, each channel uses a 4-bit
    // gain value. 0..8 maps to 0..24 dB in 3 dB steps.
    const uint8_t gain = (uint8_t)(level * 8.0f + 0.5f);
    const uint8_t reg = (uint8_t)((gain << 4) | gain);
    return writeReg(REG_ADCCONTROL1, reg);
}

bool AudioControlES8388::inputSelect(int input) {
    if (!initialized_) {
        return false;
    }

    uint8_t inputConfig = 0;
    switch (input) {
        case AUDIO_INPUT_MIC:
            // Intended Audio Kit wiring: microphone signal on LIN1/RIN1.
            inputConfig = 0x00;
            break;
        case AUDIO_INPUT_LINEIN:
            // Line-in is wired to LIN2/RIN2 on the standard board layout.
            inputConfig = 0x50;
            break;
        default:
            return false;
    }

    // Configure the ADC for the same 16-bit Philips I2S format and
    // 256 * Fs clocking used by AudioI2SPort. The ADC is kept powered down
    // until the input mux, gain, format and digital volume are configured.
    bool ok = writeReg(REG_ADCPOWER, 0xFF);
    ok &= writeReg(REG_ADCCONTROL1, 0x77); // 21 dB PGA gain, both channels.
    ok &= writeReg(REG_ADCCONTROL2, inputConfig);
    ok &= writeReg(REG_ADCCONTROL3, 0x02);
    ok &= writeReg(REG_ADCCONTROL4, 0x0C); // 16-bit I2S.
    ok &= writeReg(REG_ADCCONTROL5, 0x02); // Single speed, MCLK/LRCK = 256.
    ok &= writeReg(REG_ADCCONTROL8, 0x00); // ADC digital volume: 0 dB, left.
    ok &= writeReg(REG_ADCCONTROL9, 0x00); // ADC digital volume: 0 dB, right.

    if (ok) {
        ok = writeReg(REG_ADCPOWER, 0x09); // Power up ADC and analog inputs.
    }

    if (!ok) {
        writeReg(REG_ADCPOWER, 0xFF);
        adcEnabled_ = false;
        return false;
    }

    adcEnabled_ = true;
    return true;
}
