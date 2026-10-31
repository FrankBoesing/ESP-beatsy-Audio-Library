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
  as a starting point for an Arduino/PlatformIO sketch. If `AudioInputUSB` or
  `AudioOutputUSB` is present, it explains the required native-USB PlatformIO flags;
  the generated nodes use the library-managed constructors. At `AudioMemory()`, the
  library configures one shared `USBAudioCard`, starts USB audio and CDC, and routes
  interface-enable events to the matching node. The sketch does not need explicit
  USB includes, a card object, `usbAudio.begin()`, `USB.begin()` or a custom event
  handler. The USB environment enables `AUDIO_USB_REDIRECT_SERIAL`, so ordinary `Serial.print()` targets managed CDC; `Serial0` remains UART0. Runtime ESP-IDF logs are also redirected while the host terminal is connected.
  `AudioInputUSB` owns the `USBAudioCard::onData()` speaker callback, while
  `AudioOutputUSB` feeds the TinyUSB FIFO automatically from `AudioTask`. Neither
  direction needs regular polling in `loop()`.
- `AudioInputUSB`: experimental stereo-only, 16-bit USB playback input from a PC
  through `UAC_SPK_STEREO`. The default constructor uses managed USB startup and
  CDC; no event handler is required. The host and graph must use the same compile-time
  sample rate; resampling and more than two channels are not supported. A working
  ESP32-S3 example is `examples/HardwareTesting/USBHostPlayback/USBHostPlayback.ino`.
- `AudioOutputUSB`: experimental stereo-only, 16-bit USB capture output from the
  ESP32 to a host through `UAC_MIC_STEREO`. Its default constructor uses the same
  managed USB device; `AUDIO_USB_REDIRECT_SERIAL` maps ordinary `Serial` output to
  managed CDC while preserving `Serial0` for UART0. The graph and host sample rates must match; resampling and more than two
  channels are not supported. Explicit-`USBAudioCard` constructors remain available
  when an application needs manual control.
- Drum synthesizers: `drum-synths.md` documents the sample-free `AudioSynthKick`, `AudioSynthTom` and `AudioSynthSnare` voices. The sequenced `examples/Synthesis/AnalogDrumKit/AnalogDrumKit.ino` demonstrates their use with velocity accents, hi-hats and tom fills.
- Biquad Calculator: standalone offline tool for low-pass, high-pass, band-pass, notch, peaking EQ and shelf filters. It plots the response and exports normalized Q30 coefficients and `AudioFilterBiquad` setup code.
- EQ Chain Designer: designs and plots up to four cascaded `AudioFilterBiquad` stages and exports the corresponding setup calls. It supports low-/high-pass, band-pass, notch, peaking EQ and low-/high-shelf filters.
- FFT Window Lab: offline simulator for `AudioAnalyzeFFT256` and `AudioAnalyzeFFT1024`, embedding the exact Q15 window coefficient tables from `src/utility/data_windows.c`. Plots the window, input/windowed samples and FFT spectrum; reports bin spacing, timing, coherent gain and equivalent noise bandwidth. Models FFT256's default eight-frame power averaging and FFT1024's single-frame output, but does not reproduce target-specific FFT rounding bit for bit.
- FIR Calculator: standalone offline tool for windowed-sinc low-pass, high-pass,
  band-pass and band-stop design. It plots the quantized response and exports
  signed 16-bit Q15 coefficients for `AudioFilterFIR` (up to 199 odd-numbered taps).
- Audio Tools (`tools.html`): repairs unfinished `AudioRecordFile` `.tmp` recordings by recalculating WAV chunk lengths, converts mono/stereo 16-bit PCM WAV files into packed constant sample tables for `AudioPlayMemory`, and converts MIDI files with the ESP-beatsy MIDITONES V2.0 browser implementation. The MIDI tool reports the peak number of simultaneously active notes independently of the selected tone-generator limit, supports up to 32 tone generators, and emits the V2.0 3-bit command / 5-bit voice-index format. It defaults to `score[]` C/`PROGMEM` output and supports strategy, channel-mask, key-shift, velocity, binary, parse-only and logging options. All processing is local in the browser.
- SF2 Converter / Player (`tools.html` + `sf2-tool.js`): reads SoundFont 2 files locally, resolves preset/instrument global and local zones, generates a C++ header for the legacy `AudioSynthWavetable::instrument_data` API and includes a two-octave interactive keyboard. Play with mouse, touch or the German QWERTZ key map; held keys send Note On/Note Off with the SF2 volume envelope. The default checked compatibility mode approximates one current `AudioSynthWavetable` instance: monophonic, one sample zone per note at the selected fixed velocity, mono-downmixed samples, default amplitude 90/127 and the legacy 65,535-frame index limit. It deliberately retains loop-until-release looping during release, as the current engine does. Unchecking the mode enables a browser-only polyphonic preview with up to 16 notes and up to eight sample layers per note; this is not a complete SF2 synth. The preview cache and per-sample buffer size are bounded. V1 export still bakes one selected MIDI velocity into the generated instrument and rejects overlapping zones rather than discarding them. Linked stereo samples are downmixed to mono; optional `sm24` low-byte data is rounded to 16-bit PCM. SF2 modulators, filters, pan and effects sends are reported but not rendered. Processing is local in the browser.
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

- Hybrid handpan synthesizer: `handpan-synth.md` documents `AudioSynthHandpan`, the built-in soft attack, modal T60/gain parameters, optional sample ownership and polyphony limits. It also describes the one-shot SD_MMC WAV recorder build.
