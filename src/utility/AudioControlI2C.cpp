#include <Arduino.h>
#include "AudioControlI2C.h"

#include <esp32-hal-i2c.h>
#pragma GCC optimize("Os")

namespace AudioControlI2C {

bool isInitialized(TwoWire &wire) {
    return i2cIsInit(wire.getBusNum());
}

void begin(TwoWire &wire, int sda, int scl, uint32_t frequency) {
    if (!isInitialized(wire)) {
        wire.begin(sda, scl, frequency);
    }
}

bool isConnected(TwoWire &wire, uint8_t address) {
    wire.beginTransmission(address);
    return wire.endTransmission() == 0;
}

bool writeReg(TwoWire &wire, uint8_t address, uint8_t reg, uint8_t value) {
    wire.beginTransmission(address);
    wire.write(reg);
    wire.write(value);
    return wire.endTransmission() == 0;
}

bool readReg(TwoWire &wire, uint8_t address, uint8_t reg, uint8_t &value) {
    wire.beginTransmission(address);
    wire.write(reg);

    if (wire.endTransmission(false) != 0) {
        return false;
    }

    if (wire.requestFrom((int)address, 1) != 1) {
        return false;
    }

    value = wire.read();
    return true;
}

bool writeRegisterSequence(TwoWire &wire, uint8_t address,
                           const RegisterValue *sequence, size_t count,
                           bool stopOnError) {
    bool success = true;

    for (size_t i = 0; i < count; ++i) {
        if (!writeReg(wire, address, sequence[i].reg, sequence[i].value)) {
            success = false;
            if (stopOnError) {
                return false;
            }
        }
    }

    return success;
}

} // namespace AudioControlI2C
