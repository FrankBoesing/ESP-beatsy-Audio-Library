#include "control_es8388.h"
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
    : pins_(pins), wire_(&Wire), i2cAddress_(ES8388_ADDRESS), initialized_(false) {}

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

    if (pins_.pa_enable >= 0) {
        pinMode(pins_.pa_enable, OUTPUT);
        digitalWrite(pins_.pa_enable, !pins_.pa_active); //pa off
    }

    wire_->begin(pins_.sda, pins_.scl, ES8388_I2C_FREQUENCY);

    if (!isConnected()) {
        return false;
    }

    struct RegisterValue {
        uint8_t reg;
        uint8_t value;
    };

    static constexpr RegisterValue initSequence[] = {// DAC unmute / control
                                                     {REG_DACCONTROL3, 0x04},

                                                     // Chip Control
                                                     {REG_CONTROL2, 0x50},

                                                     // Chip Power
                                                     {REG_CHIPPOWER, 0x00},

                                                     // ESP32 ist I2S-Master -> ES8388 ist Slave
                                                     {REG_MASTERMODE, 0x00},

                                                     // DAC Power
                                                     {REG_DACPOWER, 0x3E},

                                                     // Control 1
                                                     {REG_CONTROL1, 0x12},

                                                     // DAC: 16 Bit, I2S, MCLK/FS = 256
                                                     {REG_DACCONTROL1, 0x18},
                                                     {REG_DACCONTROL2, 0x02},

                                                     // DAC routing
                                                     {REG_DACCONTROL16, 0x1B},
                                                     {REG_DACCONTROL17, 0x90},
                                                     {REG_DACCONTROL20, 0x90},
                                                     {REG_DACCONTROL21, 0x80},
                                                     {REG_DACCONTROL23, 0x00},

                                                     // DAC main volume
                                                     {REG_DACCONTROL5, 0x00},
                                                     {REG_DACCONTROL4, 0x00},

                                                     // Output volume
                                                     {REG_DACCONTROL24, 0x1E},
                                                     {REG_DACCONTROL25, 0x1E},
                                                     {REG_DACCONTROL26, 0x1E},
                                                     {REG_DACCONTROL27, 0x1E},

                                                     // DAC einschalten
                                                     {REG_DACPOWER, 0x3C}};

    for (const auto &entry : initSequence) {
        if (!writeReg(entry.reg, entry.value)) {
            return false;
        }
    }

    // Let the DAC outputs settle while the external amplifier is muted.
    delay(50);

    if (!writeReg(REG_DACCONTROL3, 0x00)) {
        return false;
    }

    // Power Amplifier des Audio-Kit-Boards einschalten
    if (pins_.pa_enable >= 0) {
        // The I2S stream is already running with silence; let the DAC ramp before enabling the PA.
        delay(50);
        digitalWrite(pins_.pa_enable, pins_.pa_active); //pa on;
    }

    initialized_ = true;
    bool ok = volume (0.7f);
    return ok;
}

bool AudioControlES8388::disable() {

    bool ok = true;

    if (initialized_) {
        if (!mute()) return false;
        ok &= !writeReg(REG_DACPOWER, 0xC0);
        ok &= !writeReg(REG_CHIPPOWER, 0xFF);
    }

    if (pins_.pa_enable >= 0) {
        pinMode(pins_.pa_enable, OUTPUT);
        digitalWrite(pins_.pa_enable, !pins_.pa_active);
    }

    initialized_ = false;
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

bool AudioControlES8388::inputLevel(float) {
    return false;
}

bool AudioControlES8388::inputSelect(int) {
    return false;
}
