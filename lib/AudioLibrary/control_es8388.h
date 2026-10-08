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
        int8_t sda;
        int8_t scl;
        int8_t pa_enable; // -1 = no PA
        bool pa_active; //active high/low
    };

    //AudioControlES8388();
    explicit AudioControlES8388(const Pins &pins);

    bool enable() override;
    bool disable() override;

    bool isConnected();

    bool volume(float level) override;
    bool volume(float left, float right);

    bool inputLevel(float volume) override;
    bool inputSelect(int n) override;

    bool mute();
    bool unmute();

    uint8_t address() const {
        return i2cAddress_;
    }

#include "control_es8388_regs.h"

  private:
    static constexpr uint32_t ES8388_I2C_FREQUENCY = 100000;
    static constexpr uint8_t ES8388_ADDRESS = 0x10;

    bool writeReg(uint8_t reg, uint8_t value);
    bool readReg(uint8_t reg, uint8_t &value);
    bool setDacVolume(uint8_t left, uint8_t right);

    Pins pins_;
    TwoWire *wire_;
    uint8_t i2cAddress_;
    bool initialized_;
};
