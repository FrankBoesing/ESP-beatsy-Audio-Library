# I²S and S/PDIF audio output

This document covers the I²S transport in `lib/AudioLibrary` and the software
S/PDIF output built on its TX serializer.

## I²S architecture

The I²S implementation uses the ESP-IDF standard driver (`driver/i2s_std.h`)
and keeps Teensy-compatible `AudioStream` blocks as the public graph interface.

- `AudioI2SPort` owns controller/channel allocation, GPIOs, DMA callbacks,
  start/stop lifecycle, and sample-rate reconfiguration.
- `AudioOutputI2S` queues pairs of 16-bit audio blocks and writes them from a
  dedicated FreeRTOS task.
- `AudioInputI2S` receives DMA blocks into a four-block ring and exposes the
  left and right I²S slots as AudioStream outputs 0 and 1.
- `AudioInputI2S2` / `AudioOutputI2S2` select ESP-IDF controller index 1.
- `AudioInputI2S3` / `AudioOutputI2S3` select ESP-IDF controller index 2.

The standard PCM transport currently uses Philips I²S, stereo, 16-bit slots and
MCLK = 256 × the configured sample rate when MCLK is routed. TDM is not
implemented. Controller availability depends on the target SoC; the ESP32 and
ESP32-S3 have two I²S controllers, while ESP32-P4 has three. The number in a
class name does not guarantee that the target has that physical controller.

### Pin assignment

Provide the actual board wiring to the relevant constructor. The standard
I²S classes take BCLK, WS/LRCLK, DOUT or DIN and optional MCLK. Input and output
can share one controller for full-duplex operation, but their BCLK, WS and MCLK
settings must match. Do not instantiate two outputs or two inputs on the same
controller.

The library has one global AudioStream sample rate and one graph scheduler.
Active I²S controllers therefore share the same configured sample rate. A
single active I²S direction owns the external scheduler clock; other I²S
callbacks transfer data without advancing the global graph a second time.

### RX diagnostics

The I²S input classes expose:

- `rxBlockCount()`: received DMA callbacks.
- `rxErrorCount()`: callbacks with unexpected DMA block size.
- `droppedBlockCount()`: blocks dropped because the software ring is full or
  AudioStream memory allocation fails.

## PDM output

`AudioOutputPDM` sends 16-bit PCM blocks to the ESP-IDF PDM TX driver and uses
the SoC's hardware PCM-to-PDM converter. The I²S/DMA hardware generates the
high-rate PDM stream; the AudioStream processing loop handles PCM blocks only.

- Uses I²S0 in PCM-to-PDM direct-DAC mode.
- Supports mono output on one DATA pin and stereo output on two DATA pins when
  the target exposes the PDM two-line DAC mode. The public stereo pin order is
  `dataLeft, dataRight`; ESP-IDF routes the left PCM slot to DOUT2 and the
  right PCM slot to DOUT, so the class maps these pins explicitly.
- No PDM clock pin is needed at the external connector in direct-DAC mode.
- PDM is a high-frequency 1-bit stream, not an analog audio voltage. Use a
  suitable low-pass filter before the amplifier; do not connect a speaker
  directly to an ESP32 GPIO.
- PDM controller mode and normal I²S0 TX are mutually exclusive; do not run
  `AudioOutputPDM`, `AudioOutputI2S`, or `AudioOutputSPDIF1` simultaneously.
- Accepts PCM sample rates from 16 kHz through 48 kHz.
- The DAC clock uses `up_sample_fs = 480`, the documented alternative DAC mode
  in which the PDM bit clock is `128 × PCM sample rate`. This mode sounded
  clean in the direct hardware comparison at 22.05 kHz; initialization and
  runtime sample-rate reconfiguration use the same setting.
- Runtime sample-rate changes pause AudioStream graph updates, wait for the PDM
  TX writer, discard queued old-rate blocks, and then reconfigure the PDM clock.
- ESP32-S3 supports PCM-to-PDM on I²S0 and two PDM DAC lines. The original ESP32
  does not expose the same two-line direct-DAC mode. Targets without the needed
  ESP-IDF/SoC feature combination are reported as unsupported.

### Mono usage

```cpp
#include "Audio.h"

AudioOutputPDM pdmOut(PDM_DATA_PIN);

// Connect an AudioStream source output to input 0 of pdmOut.

void setup() {
    pdmOut.setSampleRate(44100.0f);
    AudioMemory(12);
    pdmOut.begin();
}

void loop() {}
```

### Stereo usage

```cpp
#include "Audio.h"

AudioOutputPDM pdmOut;

void setup() {
    pdmOut.setSampleRate(44100.0f);
    AudioMemory(12);
    pdmOut.begin(PDM_DATA_LEFT, PDM_DATA_RIGHT);
    // Connect left/right AudioStream sources to inputs 0 and 1.
}

void loop() {}
```

The class exposes `begin()`, `begin(pin)`, `begin(left, right)`,
`begin(Pins)`, `end()`, `isRunning()`, `channels()`,
`setChannelCount()`, and `setSampleRate()`. Passing two pins to the
constructor or to `begin(left, right)` selects stereo automatically.

Reference: [ESP-IDF I²S PDM TX mode](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/peripherals/i2s.html#pdm-tx-mode-in-pcm-format-with-pcm-to-pdm-converter).

## S/PDIF output 1

`AudioOutputSPDIF1` is an output-only IEC 60958 / consumer S/PDIF encoder. It
uses the I²S controller 0 serializer but performs the S/PDIF Biphase Mark Code
(BMC) encoding in software. It is separate from normal `AudioOutputI2S` mode;
both cannot use I²S controller 0 simultaneously.

### Data format and timing

The class accepts stereo 16-bit PCM blocks from AudioStream. For every
left/right sample frame it creates two S/PDIF subframes:

- Subframes start with the B/M (left channel) or W (right channel) BMC preamble.
- The 16-bit sample is MSB-aligned in the 20-bit audio field; the remaining low
  auxiliary bits are zero.
- Validity and user bits are zero, the channel-status bit describes consumer
  PCM, sample rate and 16-bit word length, and the parity bit makes bits 4–31
  even-parity.
- The 192-frame S/PDIF channel-status block is tracked across AudioStream blocks.

Each BMC subframe has 64 half-bit states. Therefore one stereo PCM frame produces
128 serial states. The I²S serializer is configured for 16-bit stereo slots at
4 × the audio sample rate. This gives BCLK = 128 × Fs while DMA carries one
AudioStream block per completion period. BCLK and WS are generated internally
but are not routed to GPIOs; only the DATA pin is needed externally.

Use 44.1 kHz or 48 kHz for the first hardware tests. Channel status frequency
codes are provided for common IEC 60958 rates; other rates are marked as
unspecified, and the physical I²S clock must still be supported by the target.

### Example

```cpp
#include "Audio.h"

AudioOutputSPDIF1 spdifOut(PIN_SPDIF_OUT);
// Connect a stereo AudioStream source to spdifOut inputs 0 and 1.

void setup() {
    spdifOut.setSampleRate(44100.0f);
    // Set up AudioMemory() and the source/patch cords before starting playback.
}

void loop() {}
```

`AudioOutputSPDIF1` also accepts `AudioOutputSPDIF1::Pins{PIN_SPDIF_OUT}` and
provides `begin()`, `begin(pin)`, `end()`, `isRunning()`,
`txCallbackCount()`, and `setSampleRate()`. The output uses I²S controller 0
and is not an independent sample-rate graph.

### Electrical interface

The GPIO produces a logic-level BMC bitstream, not a guaranteed IEC 60958 coaxial
line level. A compliant consumer coax output needs suitable line-driver and
impedance/transformer circuitry. An optical transmitter module must accept the
GPIO's logic level and the approximately 5.64 MHz bit clock at 44.1 kHz audio.

### Not implemented yet

- S/PDIF input / receiver.
- S/PDIF outputs on I²S controller 1 and 2 (planned as later variants).
- TDM.
- Automated hardware validation across ESP32 variants.
