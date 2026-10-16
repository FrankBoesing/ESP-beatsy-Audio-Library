# ESP-beatsy Audio System Design Tool

This directory contains the offline Audio System Design Tool, adapted from the
Teensy Audio System Design Tool for the ESP-beatsy Audio Library.

- Start: open `index.html` in a modern browser.
- Node palette: includes ESP-beatsy classes for I2S controllers, S/PDIF, PDM,
  ES8311/ES8388 codec control, MP3/AAC playback, AudioMixerN and WAV recording.
- Codec node help documents the ES8388 ALC/limiter and noise-gate APIs, the ES8311
  ALC, noise-gate and ADC high-pass APIs, and the default hardware DAC soft-ramp
  behavior. The GUI describes these APIs but does not generate codec setup calls.
  Both codec drivers ramp their DAC output from mute to a default volume of `0.2`
  during `enable()`; applications can set another level explicitly afterward.
- Help for `AudioEffectDigitalCombine`, `AudioEffectMultiply`, `AudioEffectRectifier`,
  `AudioEffectMidSide`, `AudioEffectWaveFolder` and `AudioEffectWaveshaper` documents
  their block-wise `bypass(bool)` API. A bypass setting is read at the start of each
  audio block and forwards the primary input unchanged (both inputs for MidSide).
- Export: generates the Audio graph declarations and `AudioConnection` lines
  as a starting point for an Arduino/PlatformIO sketch.
- Biquad Calculator: standalone offline tool for low-pass, high-pass, band-pass, notch, peaking EQ and shelf filters. It plots the response and exports normalized Q30 coefficients and `AudioFilterBiquad` setup code.
- EQ Chain Designer: designs and plots up to four cascaded `AudioFilterBiquad` stages and exports the corresponding setup calls. It supports low-/high-pass, band-pass, notch, peaking EQ and low-/high-shelf filters.
- FFT Window Lab: offline simulator for `AudioAnalyzeFFT256` and `AudioAnalyzeFFT1024`, embedding the exact Q15 window coefficient tables from `lib/AudioLibrary/utility/data_windows.c`. Plots the window, input/windowed samples and FFT spectrum; reports bin spacing, timing, coherent gain and equivalent noise bandwidth. Models FFT256's default eight-frame power averaging and FFT1024's single-frame output, but does not reproduce target-specific FFT rounding bit for bit.
- FIR Calculator: standalone offline tool for windowed-sinc low-pass, high-pass,
  band-pass and band-stop design. It plots the quantized response and exports
  signed 16-bit Q15 coefficients for `AudioFilterFIR` (up to 199 odd-numbered taps).
- Audio Tools (`tools.html`): repairs unfinished `AudioRecordFile` `.tmp` recordings by recalculating WAV chunk lengths, and converts mono/stereo 16-bit PCM WAV files into packed constant sample tables for `AudioPlayMemory`. All processing is local in the browser.
- This tool remains a work in progress. It is an offline GUI for graph design,
  not a complete parser or code generator for every library API.

## Important notes

The graphical node metadata describes port counts and names for visualization.
A class appearing in the palette does not itself guarantee that its constructor,
pin configuration, or initialization calls are automatically generated. In
particular, ESP-beatsy I/O classes typically need explicit pin configuration
and `begin()` calls in sketch code. `AudioMixerN` is represented as a
32-input visual node because its channel count is selected by its constructor;
adjust the generated declaration to the number of inputs used in your graph.

The upstream files and their copyright/license notices are preserved. The
Node-RED-derived files identify the Apache License 2.0; bundled dependencies
retain their own license notices. See `LICENSE-APACHE-2.0.txt` for the full
Apache license text.

- Hybrid handpan synthesizer: `handpan-synth.md` documents `AudioSynthHandpan`, modal T60/gain parameters, sample ownership, polyphony limits and the test example.
