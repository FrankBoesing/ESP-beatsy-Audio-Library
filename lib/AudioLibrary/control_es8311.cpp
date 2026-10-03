#include "control_es8311.h"

#pragma GCC optimize("Os")

namespace {
constexpr uint32_t ES8311_I2C_FREQUENCY = 400000;
}

// Coefficients for MCLK = 256 * sample rate, taken from the clock divider
// table in Espressif's es8311.c (entries filtered to the 256x MCLK case).
constexpr AudioControlES8311::ClockCoeff AudioControlES8311::CLOCK_COEFFS[] = {

    // rate  pre_div pre_multi adc_div dac_div fs_mode lrck_h lrck_l bclk_div adc_osr dac_osr

    { 8000,  0x01, 0x00, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x20 },
    {11025,  0x01, 0x00, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x20 },
    {12000,  0x01, 0x00, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x20 },
    {16000,  0x01, 0x00, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x20 },

    {22050,  0x01, 0x00, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10 },
    {24000,  0x01, 0x00, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10 },
    {32000,  0x01, 0x00, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10 },
    {44100,  0x01, 0x00, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10 },
    {48000,  0x01, 0x00, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10 },
    {64000,  0x01, 0x00, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10 },
};

AudioControlES8311::AudioControlES8311()
    : pins_{16, 15, 1}, wire_(&Wire), i2cAddress_(ES8311_ADDRESS),
      initialized_(false) {}

AudioControlES8311::AudioControlES8311(const Pins &pins)
    : pins_(pins), wire_(&Wire), i2cAddress_(ES8311_ADDRESS),
      initialized_(false) {}

bool AudioControlES8311::writeReg(uint8_t reg, uint8_t value) {
    wire_->beginTransmission(i2cAddress_);
    wire_->write(reg);
    wire_->write(value);
    return wire_->endTransmission() == 0;
}

bool AudioControlES8311::readReg(uint8_t reg, uint8_t &value) {
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

bool AudioControlES8311::isConnected() {
    wire_->beginTransmission(i2cAddress_);
    return wire_->endTransmission() == 0;
}

bool AudioControlES8311::configureClock(uint32_t sampleRate) {
    const ClockCoeff *coeff = nullptr;
    for (size_t i = 0; i < CLOCK_COEFFS_COUNT; i++) {
        if (CLOCK_COEFFS[i].rate == sampleRate) {
            coeff = &CLOCK_COEFFS[i];
            break;
        }
    }
    if (coeff == nullptr) {
        return false;
    }

    bool ok = true;

    // Register 0x01: MCLK from MCLK pin, enable all internal clocks.
    ok &= writeReg(REG_CLK_MANAGER1, 0x3F);

    // Register 0x02: pre-divider / pre-multiplier.
    uint8_t reg02 = 0;
    readReg(REG_CLK_MANAGER2, reg02);
    reg02 &= 0x07;
    reg02 |= (coeff->pre_div - 1) << 5;
    reg02 |= coeff->pre_multi << 3;
    ok &= writeReg(REG_CLK_MANAGER2, reg02);

    // Register 0x03: ADC fsmode + osr.
    ok &= writeReg(REG_CLK_MANAGER3, (coeff->fs_mode << 6) | coeff->adc_osr);

    // Register 0x04: DAC osr.
    ok &= writeReg(REG_CLK_MANAGER4, coeff->dac_osr);

    // Register 0x05: ADC/DAC clock dividers.
    ok &= writeReg(REG_CLK_MANAGER5, ((coeff->adc_div - 1) << 4) | (coeff->dac_div - 1));

    // Register 0x06: BCLK divider (no inversion).
    uint8_t reg06 = 0;
    readReg(REG_CLK_MANAGER6, reg06);
    reg06 &= 0xE0;
    reg06 |= (coeff->bclk_div < 19) ? (coeff->bclk_div - 1) : coeff->bclk_div;
    ok &= writeReg(REG_CLK_MANAGER6, reg06);

    // Register 0x07: LRCK divider high byte.
    uint8_t reg07 = 0;
    readReg(REG_CLK_MANAGER7, reg07);
    reg07 &= 0xC0;
    reg07 |= coeff->lrck_h;
    ok &= writeReg(REG_CLK_MANAGER7, reg07);

    // Register 0x08: LRCK divider low byte.
    ok &= writeReg(REG_CLK_MANAGER8, coeff->lrck_l);

    return ok;
}

bool AudioControlES8311::enable(uint32_t sampleRate) {
    wire_->begin(pins_.sda, pins_.scl, ES8311_I2C_FREQUENCY);

    if (!isConnected()) {
        initialized_ = false;
        return false;
    }

    bool ok = true;

    // ------------------------------------------------------------
    // ES8311-Initialisierung, portiert aus Espressifs offiziellem
    // es8311.c (es8311_init). ESP32 ist I2S-Master, ES8311 laeuft
    // als I2S-Slave; MCLK wird vom ESP32 ueber den MCLK-Pin
    // eingespeist (256 * Samplerate, 16-Bit-I2S-Slots).
    // ------------------------------------------------------------

    // Chip zuruecksetzen und wieder hochfahren
    ok &= writeReg(REG_RESET, 0x1F);
    delay(20);
    ok &= writeReg(REG_RESET, 0x00);
    ok &= writeReg(REG_RESET, 0x80);

    // Taktbaum fuer die gewuenschte Samplerate konfigurieren
    ok &= configureClock(sampleRate);

    // Serielles Format: Slave-Modus, 32 Bit (passend zu AudioOutputI2S)
    uint8_t reg00 = 0;
    readReg(REG_RESET, reg00);
    reg00 &= 0xBF;
    ok &= writeReg(REG_RESET, reg00);
    ok &= writeReg(REG_SDPIN, 0x0C);   // DAC: 16 Bit
    ok &= writeReg(REG_SDPOUT, 0x0C);  // ADC: 16 Bit

    // Analoge Stufen und Signalpfad einschalten
    ok &= writeReg(REG_SYSTEM0D, 0x01); // Analog-Bias einschalten
    ok &= writeReg(REG_SYSTEM0E, 0x02); // Analog-PGA + ADC-Modulator einschalten
    ok &= writeReg(REG_SYSTEM12, 0x00); // DAC einschalten
    ok &= writeReg(REG_SYSTEM13, 0x10); // Ausgang auf Kopfhoerer-/Line-Treiber schalten
    ok &= writeReg(REG_ADC1C, 0x6A);    // ADC-Equalizer-Bypass, DC-Offset-Korrektur
    ok &= writeReg(REG_DAC37, 0x08);    // DAC-Equalizer-Bypass

    if (!ok) {
        initialized_ = false;
        return false;
    }

    // Standardwerte: 0 dB Ausgangslautstaerke, nicht stummgeschaltet
    ok &= volume(1.0f);
    ok &= unmute();

    // Power Amplifier des Boards einschalten (aktiv HIGH)
    if (pins_.pa_enable >= 0) {
        pinMode(pins_.pa_enable, OUTPUT);
        digitalWrite(pins_.pa_enable, HIGH);
    }

    initialized_ = ok;
    return ok;
}

bool AudioControlES8311::disable() {
    bool ok = true;

    if (initialized_) {
        ok &= mute();
        ok &= writeReg(REG_SYSTEM0E, 0x00);
        ok &= writeReg(REG_RESET, 0x00);
    }

    if (pins_.pa_enable >= 0) {
        pinMode(pins_.pa_enable, OUTPUT);
        digitalWrite(pins_.pa_enable, LOW);
    }

    initialized_ = false;
    return ok;
}

bool AudioControlES8311::volume(float level) {
    if (level < 0.0f) {
        level = 0.0f;
    } else if (level > 1.0f) {
        level = 1.0f;
    }

    // DAC volume register: 0x00 = mute, 0xFF = 0 dB (linear 256-step scale).
    const uint8_t reg32 = (level <= 0.0f) ? 0x00 : static_cast<uint8_t>(level * 256.0f - 1.0f + 0.5f);
    return writeReg(REG_DAC32, reg32);
}

bool AudioControlES8311::inputLevel(float level) {
    if (level < 0.0f) {
        level = 0.0f;
    } else if (level > 1.0f) {
        level = 1.0f;
    }

    // Analog mic PGA gain: 0 (0 dB) .. 8 (42 dB) in 6 dB steps.
    const uint8_t gain = static_cast<uint8_t>(level * 8.0f + 0.5f);
    return writeReg(REG_ADC16, gain);
}

bool AudioControlES8311::inputSelect(int) {
    // ES8311 hat nur einen festen analogen Mic-/Line-Eingangspfad.
    return false;
}

bool AudioControlES8311::mute() {
    uint8_t reg = 0;
    if (!readReg(REG_DAC31, reg)) {
        return false;
    }

    reg |= 0x60; // Bit 5 + Bit 6
    return writeReg(REG_DAC31, reg);
}

bool AudioControlES8311::unmute() {
    uint8_t reg = 0;
    if (!readReg(REG_DAC31, reg)) {
        return false;
    }

    reg &= static_cast<uint8_t>(~0x60);
    return writeReg(REG_DAC31, reg);
}
