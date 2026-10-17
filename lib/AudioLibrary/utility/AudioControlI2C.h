#pragma once

#include <Wire.h>
#include <stddef.h>
#include <stdint.h>

namespace AudioControlI2C {

struct RegisterValue {
    uint8_t reg;
    uint8_t value;
};

bool isInitialized(TwoWire &wire);
void begin(TwoWire &wire, int sda, int scl, uint32_t frequency);
bool isConnected(TwoWire &wire, uint8_t address);
bool writeReg(TwoWire &wire, uint8_t address, uint8_t reg, uint8_t value);
bool readReg(TwoWire &wire, uint8_t address, uint8_t reg, uint8_t &value);

// Write a register sequence in order. By default, stop at the first failed
// write. Set stopOnError=false to attempt every write and report any failure.
bool writeRegisterSequence(TwoWire &wire, uint8_t address,
                           const RegisterValue *sequence, size_t count,
                           bool stopOnError = true);

} // namespace AudioControlI2C
