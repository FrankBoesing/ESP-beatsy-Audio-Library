# Diesel Engine Sample Player

This example uses the CC0 recording **“Diesel Start-Idle-Revs MZ000004.wav”** by DAVESTALKER:

- Source: https://freesound.org/people/DAVESTALKER/sounds/443319/
- License: CC0 (the recording page identifies it as CC0)
- Recording format: PCM WAV, mono, 44.1 kHz, 16 bit
- Reported duration: 26.613 s

## Prepare the sample

1. Download the original WAV from Freesound.
2. Copy it to the SD card using the original filename `/443319__davestalker__diesel-start-idle-revs-mz000004.wav`.
3. Use the repository's usual example workflow to compile `DieselEngine.cpp` as the active sketch (for example, copy it to the project's active `src/main.cpp`) for a board with compatible I2S output, codec pins and SPI SD-card wiring. The ESP32-Audio-Kit environment uses CS=13, MOSI=15, MISO=2, SCK=14 from `platformio.ini`.

The sample is not copied into this repository; download it from the original source. The recording description notes that there is some background talking/directing, so check the chosen cut points before using it in a finished demo.

## Controls over the serial monitor (115200 baud)

- `s`: play the start portion once, then switch to the idle loop
- `i`: loop the idle portion
- `r`: play the rev portion once, then switch to the idle loop
- `+` / `-`: increase/decrease the RPM setting by 100
- `0`: reset to the reference value (800 RPM)
- `x`: stop playback
- `?`: print the controls

The RPM control changes the sample playback rate, including its pitch. The 800 RPM reference is an approximation, not a measured RPM value for the recorded engine.

## Sample boundaries

The initial boundary times were estimated from the full 26.613 s original WAV's amplitude envelope; the cuts have **not yet been confirmed by listening for clean edit points**. They currently use 0–2.5 s as the start sequence, 2.5–8.5 s as the idle loop, and 11.5–13.5 s as a rev segment. The idle loop ends before the voice noted at about 9 s; the rev cut avoids the voice noted at about 14.8–16.5 s. Adjust `START_END_MS`, `IDLE_START_MS`, `IDLE_END_MS`, `REV_START_MS` and `REV_END_MS` near the top of `DieselEngine.cpp` to match clean sections of the downloaded recording. All values are offsets from the beginning of the original WAV.

The SD card is mounted through SPI using the pin macros from `platformio.ini`. The entire PCM payload is loaded into PSRAM where available (about 2.35 MB for the full recording). The example requires enough memory for that sample and does not stream the WAV from SD during the real-time audio callback.
