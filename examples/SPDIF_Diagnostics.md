# AudioOutputSPDIF diagnostic test

This isolated test removes radio/player/network activity from the signal path.
It generates a continuous 1 kHz sine wave at 44.1 kHz and reports TX diagnostics
once per second.

## Run it

1. Copy `SPDIF_Diagnostics.cpp` to your application's `src/main.cpp`.
2. Set `SPDIF_DATA_PIN` to the same GPIO that is connected to your S/PDIF
   transmitter/interface.
3. Build and upload with the existing Freenove ESP32-S3 PlatformIO environment.
4. Open the serial monitor at 115200 baud, let the test run for 10–20 seconds,
   and listen for interruptions.
5. Save several consecutive `SPDIF running=...` lines, including any output
   where drop/error counters increase.

No other component using I²S0 should be active during this test. Only this sine
source should feed the S/PDIF output.

## Interpreting the report

At 44.1 kHz with 128-sample AudioStream blocks, the expected rate is about
345 blocks per second.

- `queueDrops` increasing: the software TX queue is discarding queued audio.
- `writeErrors` increasing: an I²S write failed or wrote fewer bytes than expected.
- `maxEncode` approaching or exceeding 2900 us: encoding may be too slow for
  the 2.90 ms audio-block period.
- `callback` and `written` rates that consistently diverge: inspect the
  TX task, DMA buffering and queue behavior.
- If the counters remain stable with no drops or write errors but interruptions
  remain audible, inspect the electrical interface, DATA waveform, preambles,
  BMC bit order and the receiver lock.

The timing maxima are lifetime maxima since `begin()`, not per-second values.
The diagnostic test does not validate the electrical S/PDIF interface by itself.
