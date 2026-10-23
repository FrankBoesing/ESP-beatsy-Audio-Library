# Cathedral Bells (no I2S input)

This standalone example synthesizes slow, bell-like tones with `AudioSynthHandpan` and feeds them into `AudioEffectVintageReverb`. Only the I2S output and the codec are used; no physical or digital audio input is required.

The effect starts with the `LARGE_CATHEDRAL` preset, a long decay and a predominantly wet mix. Adjust `cathedral.mix()`, `cathedral.decay()`, `cathedral.predelay()` and `cathedral.size()` to explore different room impressions.

Open the serial monitor at 115200 baud to see initialization status, memory placement and periodic CPU/audio-block diagnostics. Delay memory prefers internal RAM and falls back to PSRAM.
