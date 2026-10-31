# Cathedral Bells (no I2S input)

This standalone example synthesizes slow, bell-like tones with `AudioSynthHandpan` and feeds them into `AudioEffectVintageReverb`. The post-reverb stereo signal goes both to the I2S codec output and, optionally, to an SD-card WAV recorder. No audio input is required.

The effect starts with the `LARGE_CATHEDRAL` preset, a long decay and a predominantly wet mix. Adjust `cathedral.mix()`, `cathedral.decay()`, `cathedral.predelay()` and `cathedral.size()` to explore different room impressions.

## SD recording

Recording is enabled by default. It records 60 seconds of stereo, 16-bit PCM at `AUDIO_SAMPLE_RATE_EXACT`, then finalizes the file as `/cathedral_bells.wav` on the SD card. The recording duration can be changed at the top of `CathedralBells.ino` with `CATHEDRAL_RECORD_DURATION_MS`.

To compile without SD-card support, change this macro at the top of `CathedralBells.ino`:

```cpp
#define CATHEDRAL_RECORD_TO_SD 0
```

The selected PlatformIO environment must define the board's `PIN_SD_MMC_CLK`, `PIN_SD_MMC_CMD` and `PIN_SD_MMC_D0`–`PIN_SD_MMC_D3` pins for a custom pin mapping. Recording requires PSRAM. The example defaults to 1-bit SD_MMC mode for robustness; set `CATHEDRAL_SDMMC_1BIT` to `0` to use the full 4-bit bus. If mounting or recording fails, audio playback continues without recording.

The recorder intentionally does not overwrite an existing `/cathedral_bells.wav` or unfinished `/cathedral_bells.tmp`; rename or remove either file before recording again. If recording ends successfully, the temporary file is renamed to WAV after the audio writer drains its buffer.

Open the serial monitor at 115200 baud to see initialization status, SD/recording messages, reverb memory placement, and periodic CPU/audio-block diagnostics. The reverb delay memory prefers internal RAM and falls back to PSRAM.
