#pragma once

#include <Arduino.h>
#include <Wire.h>
#include "AudioControl.h"

/**
 * Minimal ES8388 codec control for the ESP32 Audio Kit V2.2.
 *
 * This class intentionally contains codec/I2C control only.
 * I2S data transport remains the responsibility of AudioOutputI2S.
 *
 * Verified board defaults for Ai-Thinker ESP32-A1S Audio Kit V2.2:
 *   I2C SDA  = GPIO33
 *   I2C SCL  = GPIO32
 *   ES8388 I2C address = 0x10
 *   PA enable = GPIO21
 *
 * The I2S pins are deliberately NOT defined here.
 */
class AudioControlES8388 : public AudioControl {
public:
    struct Pins {
        int8_t sda = 33;
        int8_t scl = 32;
        int8_t pa_enable = 21;
    };

    AudioControlES8388();
    explicit AudioControlES8388(const Pins& pins);

    bool enable() override;
    bool disable()override;

    bool isConnected();

    bool volume(float level) override;
    bool volume(float left, float right);

    bool inputLevel(float volume) override;
    bool inputSelect(int n) override;

    bool mute();
    bool unmute();

    uint8_t address() const { return i2cAddress_; }

private:
    static constexpr uint8_t ES8388_ADDRESS = 0x10;

    // ES8388 register addresses used by the DAC playback path.
    static constexpr uint8_t REG_CONTROL1        = 0x00;
    static constexpr uint8_t REG_CONTROL2        = 0x01;
    static constexpr uint8_t REG_CHIPPOWER       = 0x02;
    static constexpr uint8_t REG_ADCPOWER        = 0x03;
    static constexpr uint8_t REG_DACPOWER        = 0x04;
    static constexpr uint8_t REG_CHIPLOPOW1      = 0x05;
    static constexpr uint8_t REG_CHIPLOPOW2      = 0x06;
    static constexpr uint8_t REG_ANAVOLMANAG     = 0x07;
    static constexpr uint8_t REG_MASTERMODE      = 0x08;

    /* ADC */
    static constexpr uint8_t REG_ADCCONTROL1     = 0x09;
    static constexpr uint8_t REG_ADCCONTROL2     = 0x0a;
    static constexpr uint8_t REG_ADCCONTROL3     = 0x0b;
    static constexpr uint8_t REG_ADCCONTROL4     = 0x0c;
    static constexpr uint8_t REG_ADCCONTROL5     = 0x0d;
    static constexpr uint8_t REG_ADCCONTROL6     = 0x0e;
    static constexpr uint8_t REG_ADCCONTROL7     = 0x0f;
    static constexpr uint8_t REG_ADCCONTROL8     = 0x10;
    static constexpr uint8_t REG_ADCCONTROL9     = 0x11;
    static constexpr uint8_t REG_ADCCONTROL10    = 0x12;
    static constexpr uint8_t REG_ADCCONTROL11    = 0x13;
    static constexpr uint8_t REG_ADCCONTROL12    = 0x14;
    static constexpr uint8_t REG_ADCCONTROL13    = 0x15;
    static constexpr uint8_t REG_ADCCONTROL14    = 0x16;

    /* DAC */
    static constexpr uint8_t REG_DACCONTROL1     = 0x17;
    static constexpr uint8_t REG_DACCONTROL2     = 0x18;
    static constexpr uint8_t REG_DACCONTROL3     = 0x19;
    static constexpr uint8_t REG_DACCONTROL4     = 0x1a;
    static constexpr uint8_t REG_DACCONTROL5     = 0x1b;
    static constexpr uint8_t REG_DACCONTROL6     = 0x1c;
    static constexpr uint8_t REG_DACCONTROL7     = 0x1d;
    static constexpr uint8_t REG_DACCONTROL8     = 0x1e;
    static constexpr uint8_t REG_DACCONTROL9     = 0x1f;
    static constexpr uint8_t REG_DACCONTROL10    = 0x20;
    static constexpr uint8_t REG_DACCONTROL11    = 0x21;
    static constexpr uint8_t REG_DACCONTROL12    = 0x22;
    static constexpr uint8_t REG_DACCONTROL13    = 0x23;
    static constexpr uint8_t REG_DACCONTROL14    = 0x24;
    static constexpr uint8_t REG_DACCONTROL15    = 0x25;
    static constexpr uint8_t REG_DACCONTROL16    = 0x26;
    static constexpr uint8_t REG_DACCONTROL17    = 0x27;
    static constexpr uint8_t REG_DACCONTROL18    = 0x28;
    static constexpr uint8_t REG_DACCONTROL19    = 0x29;
    static constexpr uint8_t REG_DACCONTROL20    = 0x2a;
    static constexpr uint8_t REG_DACCONTROL21    = 0x2b;
    static constexpr uint8_t REG_DACCONTROL22    = 0x2c;
    static constexpr uint8_t REG_DACCONTROL23    = 0x2d;
    static constexpr uint8_t REG_DACCONTROL24    = 0x2e;
    static constexpr uint8_t REG_DACCONTROL25    = 0x2f;
    static constexpr uint8_t REG_DACCONTROL26    = 0x30;
    static constexpr uint8_t REG_DACCONTROL27    = 0x31;
    static constexpr uint8_t REG_DACCONTROL28    = 0x32;
    static constexpr uint8_t REG_DACCONTROL29    = 0x33;
    static constexpr uint8_t REG_DACCONTROL30    = 0x34;

    bool writeReg(uint8_t reg, uint8_t value);
    bool readReg(uint8_t reg, uint8_t& value);
    bool setDacVolume(uint8_t left, uint8_t right);

    Pins pins_;
    TwoWire* wire_;
    uint8_t i2cAddress_;
    bool initialized_;
};
