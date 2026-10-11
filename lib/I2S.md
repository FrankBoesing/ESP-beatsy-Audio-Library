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
the SoC's hardware PCM-to-PDM converter. This does not encode PDM bits in the
AudioStream processing loop; the I²S/DMA hardware generates the high-rate stream.

- Uses I²S0 in PCM-to-PDM direct-DAC mode.
- Supports mono output on one DATA pin and stereo output on two separate DATA
  pins, provided the target exposes the PDM two-line DAC mode. The public stereo
  pin order is `dataLeft, dataRight`; ESP-IDF routes the left PCM slot to DOUT2
  and the right PCM slot to DOUT, so the class maps these pins explicitly.
- The PDM DAC mode does not need a PDM CLK pin at the external connector.
- PDM is a high-frequency 1-bit stream, not an analog audio voltage. Use a
  low-pass filter before the amplifier; do not connect a speaker directly to
  an ESP32 GPIO. A single RC pole (for example 1 kΩ + 4.7 nF) may leave
  substantial PDM noise. For an initial test, use two cascaded RC stages:
  DATA → 1 kΩ → node A → 1 kΩ → node B → amplifier input, with 2.2 nF from
  each node to GND. Connect ESP32 and amplifier grounds. This is a starting
  point, not a substitute for a filter designed for the chosen amplifier/load.
- PDM controller mode and normal I²S0 TX are mutually exclusive; do not run
  `AudioOutputPDM`, `AudioOutputI2S`, or `AudioOutputSPDIF1` simultaneously.
- The current class accepts PCM sample rates from 16 kHz through 48 kHz.
- DAC clock configuration uses the documented fixed up-sampling mode
  `up_sample_fp = 960`, `up_sample_fs = 480`, giving a PDM bit clock of
  `128 × PCM sample rate`. The ESP-IDF DAC default instead sets
  `up_sample_fs = sample_rate / 100` for a fixed 6.144 MHz PDM clock. A direct
  hardware comparison sounded cleaner with `up_sample_fs = 480` at 22.05 kHz,
  so both initialization and runtime reconfiguration now use that mode.
  Runtime sample-rate changes pause AudioStream graph updates, wait for the PDM
  TX writer, discard queued old-rate blocks, and then reconfigure the PDM clock.
- The common ESP32-S3 has PCM-to-PDM on I²S0 and supports two PDM DAC lines.
  The original ESP32 supports PCM-to-PDM but does not expose the same
  two-line direct-DAC mode; `AudioOutputPDM` reports unsupported on targets
  without the required ESP-IDF/SoC feature combination.

### Direct ESP-IDF diagnostic

Before debugging `AudioOutputPDM`, the independent low-level test
[`examples/HardwareTesting/PDMDirectIDF.cpp`](../examples/HardwareTesting/PDMDirectIDF.cpp)
can be used to isolate the ESP-IDF PDM-DAC driver. It outputs the same 1 kHz
sine on GPIO 43 (left/DOUT2) and GPIO 44 (right/DOUT), without `AudioStream`
or the library's PDM output task. It requires a target with two-line PDM DAC
support, such as ESP32-S3. The sketch reports initialization and write errors
over `Serial`.

Note that GPIO 43 and GPIO 44 are also commonly the ESP32-S3 UART0 TX/RX pins.
Use a USB CDC/USB Serial-JTAG console or change the DATA pins if the serial
console is wired to UART0.

### Direct PDM DAC clock-mode comparison at 22.05 kHz

`examples/HardwareTesting/PDMDirectIDF22050AltClock.cpp` uses the direct
ESP-IDF driver with `up_sample_fs = 480`, which is the documented clock mode
where the PDM bit clock follows 128 × the PCM sample rate. Compare this output
with `PDMDirectIDF.cpp` built at 22.05 kHz with the unmodified DAC default
(`up_sample_fs = rate / 100`). This checks whether the default clock mode's
integer `rate / 100` setting affects the subjective sound at 22.05 kHz. It is
a diagnostic experiment only; do not change the library default unless the
hardware comparison supports the change.

### AudioStream PDM stereo test at 22.05 kHz

The test
[`examples/HardwareTesting/PDMViaAudioStreamStereo22050.cpp`](../examples/HardwareTesting/PDMViaAudioStreamStereo22050.cpp)
repeats the known 1 kHz stereo sine test at 22.05 kHz, matching the AAC stream
rate observed in the radio logs. It separates a sample-rate-specific PDM issue
from a problem that only appears with radio PCM blocks.

### Mono AudioStream dual-input diagnostic

If native ESP-IDF mono and library stereo both work, but the one-input library
mono test does not, try
[`examples/HardwareTesting/PDMViaAudioStreamMonoDualInput.cpp`](../examples/HardwareTesting/PDMViaAudioStreamMonoDualInput.cpp).
It uses the mono output on GPIO 43 but connects the same sine to both AudioStream
inputs. The mono path therefore averages two identical blocks. Compare with
`PDMMono.cpp`, which connects only input 0. This isolates the one-input
fallback from the two-input mono-mix branch.

### Direct ESP-IDF mono diagnostic

If the stereo tests sound correct but the mono library test does not, run
[`examples/HardwareTesting/PDMDirectIDFMono.cpp`](../examples/HardwareTesting/PDMDirectIDFMono.cpp).
It uses the native ESP-IDF PDM DAC in `I2S_SLOT_MODE_MONO` on GPIO 43 and
sends the same 1 kHz / 44.1 kHz sine without AudioStream. Compare it with
`PDMDirectIDF.cpp` (stereo) and `PDMMono.cpp` (library mono):
- If direct mono is noisy too, the issue is isolated to the native mono DAC
  mode/configuration or its output routing.
- If direct mono is clean but library mono is noisy, investigate the
  `AudioOutputPDM` mono PCM transfer path.

### Sample-rate caveat for waveform-synth tests

`AudioSynthWaveformSine::frequency()` calculates its phase increment using
the compile-time `AUDIO_SAMPLE_RATE_EXACT` constant (default 44.1 kHz), not the
runtime `AudioStream` sample rate. If the output rate is changed at runtime,
the generated pitch scales with the ratio
`runtime sample rate / AUDIO_SAMPLE_RATE_EXACT`. This affects waveform-synth
test tones, but not PCM samples already decoded at the negotiated sample rate.
Use `PDMViaAudioStreamStereoRuntimeRateTone.cpp` for a 1 kHz test at 22.05 kHz.

### AudioStream comparison test

After the direct ESP-IDF test succeeds, use
[`examples/HardwareTesting/PDMViaAudioStreamStereo.cpp`](../examples/HardwareTesting/PDMViaAudioStreamStereo.cpp)
to test the same two pins and a 1 kHz / 44.1 kHz sine through
`AudioOutputPDM`. This deliberately uses stereo on GPIO 43 and GPIO 44,
matching `PDMDirectIDF.cpp`, to separate a mono-mode issue from an
AudioStream/queue issue. Watch the serial callback counter and compare the
sound with the direct IDF test.

### Mono example

A runnable hardware smoke test is available at
[`examples/HardwareTesting/PDMMono.cpp`](../examples/HardwareTesting/PDMMono.cpp).
Change its DATA GPIO to an output-capable pin that is free on the selected board;
connect that pin through an RC low-pass filter to an amplifier input.

```cpp
#include "Audio.h"

AudioSynthWaveformSine sine;
AudioOutputPDM pdmOut(PDM_DATA_PIN);
AudioConnection patch(sine, 0, pdmOut, 0);

void setup() {
    pdmOut.setSampleRate(44100.0f);
    sine.frequency(1000.0f);
    sine.amplitude(0.25f);
    AudioMemory(12);
    pdmOut.begin();
}

void loop() {}
```


```cpp
#include "Audio.h"

AudioOutputPDM pdmOut(PDM_DATA_PIN);
// Connect the source's left and right outputs to pdmOut inputs 0 and 1.

void setup() {
    AudioMemory(12);
    pdmOut.setSampleRate(44100.0f);
    // Optional: this is already the default.
    pdmOut.setChannelCount(1);
    // The audio graph starts the port when the hardware output is initialized.
}

void loop() {}
```

### Stereo example

```cpp
#include "Audio.h"

AudioOutputPDM pdmOut;

void setup() {
    pdmOut.setSampleRate(44100.0f);
    AudioMemory(12);
    pdmOut.begin(PDM_DATA_LEFT, PDM_DATA_RIGHT); // Enables stereo automatically.
    // Connect separate left/right AudioStream outputs to inputs 0 and 1.
}

void loop() {}
```

The helper class exposes `begin()`, `begin(pin)`, `begin(left, right)`,
`begin(Pins)`, `end()`, `isRunning()`, `txCallbackCount()`,
`channels()`, `setChannelCount()`, and `setSampleRate()`. Passing two pins
to the constructor or to `begin(left, right)` selects stereo automatically.

For transport diagnostics, `txBlockCount()` counts PCM blocks processed by the
TX task; `lastPcmMin()`, `lastPcmMax()`, and `lastPcmMeanAbs()` expose the
range and mean absolute sample value of the last PCM block prepared for the
driver. `txWriteErrorCount()` and `txQueueDropCount()` count failed driver
writes and dropped queue pairs, respectively. These counters do not inspect
the sound after the hardware PDM modulator.

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
