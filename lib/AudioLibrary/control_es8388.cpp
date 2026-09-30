#include "control_es8388.h"

#pragma GCC optimize("Os")

namespace {
constexpr uint32_t ES8388_I2C_FREQUENCY = 100000;

// DAC main volume registers:
//   0x00 = 0 dB
//   0xC0 = approximately -96 dB
// The scale is therefore inverted.
uint8_t volumeToRegister(float level) {
    if (level <= 0.0f) {
        return 0xC0;
    }
    if (level >= 1.0f) {
        return 0x00;
    }

    const float attenuation = (1.0f - level) * 192.0f;
    return static_cast<uint8_t>(attenuation + 0.5f);
}
} // namespace

AudioControlES8388::AudioControlES8388()
    : pins_{33, 32, 21}, wire_(&Wire), i2cAddress_(ES8388_ADDRESS),
      initialized_(false) {}

AudioControlES8388::AudioControlES8388(const Pins &pins)
    : pins_(pins), wire_(&Wire), i2cAddress_(ES8388_ADDRESS),
      initialized_(false) {}

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

    if (wire_->requestFrom(static_cast<int>(i2cAddress_), 1) != 1) {
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
    wire_->begin(pins_.sda, pins_.scl, ES8388_I2C_FREQUENCY);

    if (!isConnected()) {
        initialized_ = false;
        return false;
    }

    bool ok = true;

    // ------------------------------------------------------------
    // Getestete ES8388-Initialisierung
    // Diese Registerfolge entspricht dem funktionierenden
    // direkten ES8388-Test.
    // ------------------------------------------------------------

    ok &= writeReg(REG_DACCONTROL3, 0x04);

    // Chip Control
    ok &= writeReg(REG_CONTROL2, 0x50);

    // Chip Power
    ok &= writeReg(REG_CHIPPOWER, 0x00);

    // ESP32 ist I2S-Master -> ES8388 ist Slave
    ok &= writeReg(REG_MASTERMODE, 0x00);

    // DAC Power
    ok &= writeReg(REG_DACPOWER, 0x3E);

    // Control 1
    ok &= writeReg(REG_CONTROL1, 0x12);

    // DAC: 16 Bit, I2S, MCLK/FS = 256
    ok &= writeReg(REG_DACCONTROL1, 0x18);
    ok &= writeReg(REG_DACCONTROL2, 0x02);

    // DAC routing
    ok &= writeReg(REG_DACCONTROL16, 0x1B);
    ok &= writeReg(REG_DACCONTROL17, 0x90);
    ok &= writeReg(REG_DACCONTROL20, 0x90);
    ok &= writeReg(REG_DACCONTROL21, 0x80);
    ok &= writeReg(REG_DACCONTROL23, 0x00);

    // DAC main volume
    ok &= writeReg(REG_DACCONTROL5, 0x00);
    ok &= writeReg(REG_DACCONTROL4, 0x00);

    // Output volume
    ok &= writeReg(REG_DACCONTROL24, 0x1E);
    ok &= writeReg(REG_DACCONTROL25, 0x1E);
    ok &= writeReg(REG_DACCONTROL26, 0x1E);
    ok &= writeReg(REG_DACCONTROL27, 0x1E);

    // DAC einschalten
    ok &= writeReg(REG_DACPOWER, 0x3C);

    // DAC unmute
    ok &= writeReg(REG_DACCONTROL3, 0x00);

    if (!ok) {
        initialized_ = false;
        return false;
    }

    // Power Amplifier des Audio-Kit-Boards einschalten
    if (pins_.pa_enable >= 0) {
        pinMode(pins_.pa_enable, OUTPUT);
        digitalWrite(pins_.pa_enable, HIGH);
    }

    initialized_ = true;
    return true;
}

bool AudioControlES8388::disable() {
    bool ok = true;

    if (initialized_) {
        ok &= mute();
        ok &= writeReg(REG_DACPOWER, 0xC0);
        ok &= writeReg(REG_CHIPPOWER, 0xFF);
    }

    if (pins_.pa_enable >= 0) {
        pinMode(pins_.pa_enable, OUTPUT);
        digitalWrite(pins_.pa_enable, LOW);
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

    reg &= static_cast<uint8_t>(~0x04);
    return writeReg(REG_DACCONTROL3, reg);
}

bool AudioControlES8388::inputLevel(float) {
    return false;
}

bool AudioControlES8388::inputSelect(int) {
    return false;
}
