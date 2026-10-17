# Doppler Helicopter

This example demonstrates the shared variable-playback-rate API and looping with
`AudioPlayMemory`. The WAV file is decoded and downmixed to mono by
`AudioPlaySdWav::loadToMemory()`, stored in PSRAM, and then played directly from
that sample buffer. The example's flight model changes playback rate, stereo position
and gain over time.

The helicopter approaches from the right, passes the listener, recedes to the left,
turns around at a distance, and returns along the same path.

## Sound file

Use the generated **HueyRotorLoop_v4.wav** from the Huey synthesis package, or another
short, seamless rotor loop. Copy it to the SD card as `/Helicopter.wav`.

Requirements:
- Uncompressed PCM WAV, mono or stereo
- 11025, 22050 or 44100 Hz
- 8, 16, 24 or 32 bits per sample
- The complete decoded mono sample must fit in PSRAM, with at least 64 KiB reserved
  by default for other allocations

The v4 loop is mono, 44.1 kHz, 16-bit and approximately 10 seconds long. Its decoded
sample data uses about 882 kB of PSRAM. The sample itself is not committed to this repository.

## Doppler model

The example uses a cosine flight path between a distant point on the right and a distant
point on the left. The radial component of velocity determines the playback-rate multiplier:

`rate = c / (c + radialVelocity)`

Here `c` is the speed of sound. Negative radial velocity means the source is approaching
and makes the sample play faster/higher; positive radial velocity means it is receding and
makes the sample play slower/lower. Near the closest pass the radial velocity changes sign
through zero.

The sample is continuously looped by `AudioPlayMemory`. A panner adds stereo movement and
distance-dependent gain. Because the source loop is preloaded into PSRAM, audio rendering
does not need to seek or read the SD card.

## Related API

```cpp
AudioSampleBuffer sample;
AudioPlaySdWav loader;
AudioPlayMemory player;

if (loader.loadToMemory(SD_MMC, "/Helicopter.wav", sample)) {
    player.setPlaybackRate(1.0f);
    player.play(sample, true); // loop from PSRAM without copying
}
```

The buffer must remain alive and must not be released or reloaded while the memory player
is using it. `loadToMemory()` fails without replacing the previous buffer if the sample does
not fit in available contiguous PSRAM while retaining the requested reserve.

For file-backed playback without preloading, `AudioPlaySdWav::setLoop(true)` repeats the WAV
data chunk directly. This avoids a separate RAM buffer, but file seeks at the loop boundary
remain part of the file-backed playback path.

## Hardware and build

The example uses **SD_MMC in 4-bit mode**. The clock, command and data pins are defined
as `PIN_SD_MMC_CLK`, `PIN_SD_MMC_CMD` and `PIN_SD_MMC_D0` through
`PIN_SD_MMC_D3` in `platformio.ini`. The legacy SPI SD pin definitions remain there
for examples that have not yet been migrated.

For the **ESP32-A1S Audio Kit**, set the onboard SD-card switches labelled **CMD** and
**DATA3** to ON before testing. For the **Freenove ESP32-S3 Display 2.8-inch
(FNK0104A)**, the SD_MMC pin definitions use the board's documented wiring.

Build with the corresponding PlatformIO environment and copy `HueyRotorLoop_v4.wav`
to the card renamed to `/Helicopter.wav`.

This change has not been compiled locally or tested on physical hardware yet.
