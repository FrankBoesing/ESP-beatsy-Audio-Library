# I²S audio transport

This document describes the ESP-IDF I²S implementation in `lib/AudioLibrary`,
including the `AudioInputI2S[2|3]` and `AudioOutputI2S[2|3]` classes.

## Architecture

The implementation uses the ESP-IDF standard I²S driver (`driver/i2s_std.h`),
managed by `AudioI2SPort`. The Teensy-compatible `AudioStream` API remains the
audio graph interface; I²S DMA handling is kept inside the transport classes.

- `AudioI2SPort` owns controller allocation, TX/RX channels, GPIO assignment,
  callbacks, start/stop lifecycle, and sample-rate reconfiguration.
- `AudioOutputI2S` queues audio-block pairs and writes them from a dedicated
  FreeRTOS task. AudioStream samples are 16-bit; TX packs left/right samples
  into 16-bit I²S slots.
- `AudioInputI2S` copies each DMA block into a four-block single-producer /
  single-consumer ring. Its AudioStream outputs are channel 0 (left slot) and
  channel 1 (right slot). The RX counters can help diagnose errors and drops.
- The transport currently implements Philips-standard PCM with two 16-bit slots.
  TDM and SPDIF/BMC are reserved profiles, not implemented audio transports.

## Controller classes

The numbered classes are convenience wrappers around the same transport code.
They select a fixed ESP-IDF hardware controller index:

| Class | ESP-IDF controller | Index |
|---|---|---:|
| `AudioInputI2S`, `AudioOutputI2S` | `I2S_NUM_0` | 0 |
| `AudioInputI2S2`, `AudioOutputI2S2` | `I2S_NUM_1` | 1 |
| `AudioInputI2S3`, `AudioOutputI2S3` | `I2S_NUM_2` | 2 |

Controller names are library API names; they do not guarantee that the target
chip physically has that many I²S peripherals. In the current ESP-IDF
documentation, the original ESP32 and ESP32-S3 provide two I²S peripherals,
while ESP32-P4 provides three. Consequently, `AudioInputI2S3` and
`AudioOutputI2S3` compile as convenience classes but cannot start on targets
without `I2S_NUM_2`; the driver will reject the unsupported controller.

References:
- ESP32 I²S peripheral: <https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/peripherals/i2s.html>
- ESP32-S3 I²S peripheral: <https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/peripherals/i2s.html>
- ESP32-P4 I²S peripheral: <https://docs.espressif.com/projects/esp-idf/en/stable/esp32p4/api-reference/peripherals/i2s.html>

The numbered wrappers force their controller index both when constructed and
when `begin(pins)` is called. They accept the same pin structure and expose
the same public controls and diagnostics as their base class.

## Pin assignment and duplex operation

Pins are target- and board-specific. Pass the actual pin assignment explicitly,
for example:

```cpp
AudioOutputI2S2 i2sOut2({BCLK2_PIN, WS2_PIN, DOUT2_PIN, MCLK2_PIN});
AudioInputI2S2  i2sIn2 ({BCLK2_PIN, WS2_PIN, DIN2_PIN,  MCLK2_PIN});
```

The four values are BCLK, WS/LRCLK, DOUT or DIN, and optional MCLK. A value of
`-1` disables an optional MCLK output. The input and output sharing one
controller must use the same BCLK, WS and MCLK configuration; their DIN and
DOUT are separate data pins. Not every board routes every peripheral signal to
every GPIO, so check the target's GPIO matrix and board schematic.

Each controller has one TX direction and one RX direction in this implementation.
An input and output can share the same controller as full-duplex I²S. Do not
create two separate output instances on the same controller, or two separate
input instances on the same controller.

If the target lacks a suitable default pinout, the no-argument constructors leave
pins invalid intentionally. Supply pins in the constructor or call `begin(pins)`.

## Clocking and AudioStream scheduling

- I²S controllers run as clock masters. Standard PCM uses 16-bit left/right slots
  and `MCLK = 256 × sample rate` when MCLK is routed.
- The audio library currently has one global `AudioStream` sample rate and one
  graph scheduler. Multiple active I²S controllers therefore share the same
  configured sample rate; they are not independent-rate audio graphs.
- A single active I²S direction is selected as the scheduler clock owner. Other
  active I²S callbacks transfer data but do not tick the global audio graph a
  second time.
- Changing the sample rate through `AudioI2SPort::setSampleRate()` reconfigures
  all allocated I²S controllers together.

For mono DACs such as ES8311, call `setChannelCount(1)` on the associated
output. It mixes the AudioStream left/right inputs to mono and writes the same
sample to both I²S slots.

## RX diagnostics

`AudioInputI2S`, `AudioInputI2S2`, and `AudioInputI2S3` expose:

- `rxBlockCount()`: receive callbacks processed.
- `rxErrorCount()`: callbacks with unexpected or invalid DMA block size.
- `droppedBlockCount()`: blocks dropped because the software ring is full or
  AudioStream memory allocation fails.

Counts reset when `begin()` starts the input.

## Example

```cpp
#include "Audio.h"

AudioOutputI2S2 i2sOut({BCLK_PIN, WS_PIN, DOUT_PIN, MCLK_PIN});
AudioInputI2S2  i2sIn ({BCLK_PIN, WS_PIN, DIN_PIN,  MCLK_PIN});

void setup() {
    AudioMemory(12);
    i2sOut.setSampleRate(44100.0f);
    // Connect AudioStream objects to i2sIn/i2sOut before starting the port.
    // For an explicit start, call begin() after configuring the graph.
}

void loop() {}
```

The class API does not allocate additional hardware: the chip's available I²S
controllers and usable GPIO pins determine which instances can start successfully.
