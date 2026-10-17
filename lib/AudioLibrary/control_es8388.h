#pragma once

#include <Arduino.h>
#include <Wire.h>
#include "AudioControl.h"

/**
 * Minimal ES8388 codec control for the ESP32 Audio Kit V2.2.
 *
 * This class intentionally contains codec/I2C control only.
 * I2S data transport remains the responsibility of AudioOutputI2S.
 * enable() ramps DAC volume from mute to a conservative default of 0.2.
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

    // Linear amplitude level 0.0..1.0; 1.0 maps to 0 dB DAC gain.
    bool volume(float level) override;
    bool volume(float left, float right);

    bool inputLevel(float volume) override;
    bool inputSelect(int n) override;

    // ADC automatic level control (ALC), for use after inputSelect().
    // maxGain: 0..7 (-6.5..+35.5 dB); minGain: 0..7 (-12..+30 dB).
    // targetLevel: 0..15 (-16.5..-1.5 dB); hold/decay/attack: 0..15.
    // windowSize: 0..31 peak-detector window setting.
    bool alcEnable(bool enable, bool limiterMode = false);
    bool alcConfig(uint8_t maxGain, uint8_t minGain, uint8_t targetLevel,
                   uint8_t holdTime, uint8_t decayTime, uint8_t attackTime,
                   uint8_t windowSize = 6);

    // ADC noise gate: threshold code 0..31 (-76.5..-30 dB, 1.5 dB steps).
    // muteAdcOutput=true mutes the ADC stream; otherwise PGA gain is held.
    bool noiseGate(bool enable, uint8_t threshold, bool muteAdcOutput = false);

    bool mute();
    bool unmute();

    uint8_t address() const {
        return i2cAddress_;
    }

    //Codec is stereo:
    constexpr int channels() { return 2;};

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
    bool adcEnabled_;
};
