# VERBtrix-derived vintage reverb

This example routes stereo I2S line-in through `AudioEffectVintageReverb` and back to stereo I2S output.

The DSP core is taken from [VERBtrix](https://github.com/basscheffers/VERBtrix) and remains unmodified. The ESP-beatsy wrapper adapts the effect to this library's `AudioStream` API, current runtime sample rate, and ESP32 memory allocator.

Call `reverb.begin()` in `setup()` before using the effect. The internally allocated float delay memory prefers internal RAM and falls back to PSRAM. The effect returns the input unchanged if initialization fails.

The effect uses floating-point processing and approximately 268 KB of delay memory at 44.1 kHz; the exact size depends on the sample rate. Check CPU usage and audio stability on the target board before using it in a larger graph.
