#pragma once

#include <Arduino.h>
#include <Wire.h>
#include "AudioControl.h"

/**
 * ES8311 codec control for the Freenove ESP32-S3-DevKitC-1 (FNK0104).
 *
 * Register sequence ported from Espressif's official ES8311 driver
 * (es8311.c/es8311_reg.h, SPDX Apache-2.0), adapted to use Wire
 * instead of the ESP-IDF I2C driver.
 *
 * This class intentionally contains codec/I2C control only.
 * I2S data transport remains the responsibility of AudioOutputI2S,
 * which drives 32-bit I2S slots with MCLK = 256 * sample rate.
 *
 * Default pins match the [env:esp32-s3-devkitc-1] section in platformio.ini:
 *   I2C SCL       = GPIO15
 *   I2C SDA       = GPIO16
 *   PA enable     = GPIO1
 *   I2C speed     = 400 kHz
 *   ES8311 I2C address = 0x18 (CE/ADDR pin tied low)
 *
 * The I2S pins are deliberately NOT defined here.
 */
class AudioControlES8311 : public AudioControl {
  public:
    struct Pins {
        int8_t sda = 16;
        int8_t scl = 15;
        int8_t pa_enable = 1; // -1 = no PA
    };

    AudioControlES8311();
    explicit AudioControlES8311(const Pins &pins);

    // sampleRate must match AudioStream::sampleRate() / the rate
    // configured in AudioOutputI2S (MCLK = 256 * sampleRate).
    bool enable(uint32_t sampleRate);
    bool enable() override {
        return enable(44100);
    }
    bool disable() override;

    bool isConnected();

    // level 0.0 .. 1.0, maps to the ES8311 DAC volume register (0..255).
    bool volume(float level) override;

    // level 0.0 .. 1.0, maps to the ES8311 analog mic PGA gain (0..42 dB).
    bool inputLevel(float level) override;
    bool inputSelect(int n) override;

    bool mute();
    bool unmute();

    uint8_t address() const {
        return i2cAddress_;
    }

  private:
    static constexpr uint8_t ES8311_ADDRESS = 0x18;

    // ES8311 register addresses (es8311_reg.h).
    static constexpr uint8_t REG_RESET = 0x00;

    static constexpr uint8_t REG_CLK_MANAGER1 = 0x01;
    static constexpr uint8_t REG_CLK_MANAGER2 = 0x02;
    static constexpr uint8_t REG_CLK_MANAGER3 = 0x03;
    static constexpr uint8_t REG_CLK_MANAGER4 = 0x04;
    static constexpr uint8_t REG_CLK_MANAGER5 = 0x05;
    static constexpr uint8_t REG_CLK_MANAGER6 = 0x06;
    static constexpr uint8_t REG_CLK_MANAGER7 = 0x07;
    static constexpr uint8_t REG_CLK_MANAGER8 = 0x08;

    static constexpr uint8_t REG_SDPIN = 0x09;  // DAC serial digital port
    static constexpr uint8_t REG_SDPOUT = 0x0A; // ADC serial digital port

    static constexpr uint8_t REG_SYSTEM0D = 0x0D;
    static constexpr uint8_t REG_SYSTEM0E = 0x0E;
    static constexpr uint8_t REG_SYSTEM12 = 0x12;
    static constexpr uint8_t REG_SYSTEM13 = 0x13;
    static constexpr uint8_t REG_SYSTEM14 = 0x14; // analog PGA gain

    static constexpr uint8_t REG_ADC16 = 0x16; // mic gain
    static constexpr uint8_t REG_ADC1C = 0x1C; // ADC equalizer/HPF

    static constexpr uint8_t REG_DAC31 = 0x31; // mute
    static constexpr uint8_t REG_DAC32 = 0x32; // volume
    static constexpr uint8_t REG_DAC37 = 0x37; // ramp rate / equalizer bypass

    // Clock divider coefficients for MCLK = 256 * sample rate (mclk from MCLK pin).
    struct ClockCoeff {
        uint32_t rate;
        uint8_t pre_div;
        uint8_t pre_multi;
        uint8_t adc_div;
        uint8_t dac_div;
        uint8_t fs_mode;
        uint8_t lrck_h;
        uint8_t lrck_l;
        uint8_t bclk_div;
        uint8_t adc_osr;
        uint8_t dac_osr;
    };
    static const ClockCoeff CLOCK_COEFFS[];
    static constexpr size_t CLOCK_COEFFS_COUNT = 10;

    bool writeReg(uint8_t reg, uint8_t value);
    bool readReg(uint8_t reg, uint8_t &value);
    bool configureClock(uint32_t sampleRate);

    Pins pins_;
    TwoWire *wire_;
    uint8_t i2cAddress_;
    bool initialized_;
};
