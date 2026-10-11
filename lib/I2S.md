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
