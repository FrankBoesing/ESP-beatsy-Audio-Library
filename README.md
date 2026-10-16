# ESP-beatsy Audio Library

ESP-beatsy is an ESP32-oriented port and adaptation of the [Teensy Audio Library](https://www.pjrc.com/teensy/td_libs_Audio.html). The main goal is to retain the familiar Teensy programming model and public APIs wherever practical, while adapting audio scheduling, I/O, memory handling and codec support to ESP32 hardware.

**The project is work in progress.**  It is not a claim that every class has full Teensy feature parity or has been tested on every ESP32 target.

## Teensy Audio Library compatibility

The library deliberately keeps the central Teensy concepts:

- Audio processing nodes derive from **AudioStream** and are connected with **AudioConnection** patch cords.
- Sketches use **AudioMemory(n)** to provide the shared pool of audio blocks.
- The familiar 16-bit PCM audio-block model and the usual block size of **128 samples** are retained.
- Many established Teensy class names and control methods are preserved, making existing patch designs and sketches a useful starting point.

Compatibility is primarily **source/API-level compatibility**, not binary compatibility or a promise that every Teensy sketch will compile unchanged. Hardware initialization, board pins, drivers, memory requirements and some object-specific APIs differ. Features that are not listed below should not be assumed to be implemented.

### Important implementation differences

- **Scheduling:** Unlike the Teensy implementation, ESP-beatsy uses FreeRTOS tasks and ESP32 timer/I2S callback mechanisms. When I2S output is active, the TX DMA callback can drive audio updates; otherwise a software timer can provide the update clock. Timing and processor-load numbers therefore cannot be compared directly with Teensy measurements.
- **I2S hardware:** The ESP32 implementation uses the ESP-IDF I2S driver and explicit pin configuration. AudioStream samples remain signed 16-bit PCM; conversion to the configured I2S slot representation is done in the output path. Mono output can be selected with `setChannelCount(1)`.
- **Memory:** The shared AudioMemory block pool remains, but some ESP-specific objects allocate their own buffers. Delay and reverb implementations can prefer PSRAM and fall back to internal RAM, so memory consumption is not always equivalent to the Teensy implementation.
- **Mixer extension:** In addition to the familiar four-input AudioMixer4, **AudioMixerN** supports 4–32 inputs. It uses floating-point gains and accumulation, with saturation to 16-bit PCM only at the final output.
- **Filter implementation:** AudioFilterLadder replaces the Teensy/CMSIS-DSP FIR interpolation path with portable polyphase FIR routines and uses 4× internal oversampling.
- **ESP-specific playback:** MP3/AAC decoding, generic AudioSource objects and network-stream handling extend the original hardware-focused audio graph with ESP32 software-decoder functionality.
- **WAV and memory-sample playback:** `AudioPlaySdWav::setLoop()` repeats a WAV data chunk; `loadToMemory()` can decode a supported PCM WAV into a mono `AudioSampleBuffer` allocated in PSRAM after checking free and largest-block capacity while preserving a configurable reserve. `AudioPlayMemory::play(buffer, true)` plays that buffer without copying it and can loop it with variable playback rate. The buffer must outlive playback.
- **Wavetable instruments:** `AudioSynthWavetable` renders sample-based instruments from static sample/instrument data, commonly generated from an SF2 SoundFont by an external converter. It does not parse or load `.sf2` files at runtime; use selected converted instruments rather than bundling a large complete SoundFont in a sketch.
- **Hardware codecs:** AudioControlES8311 and AudioControlES8388 are included for boards using those codecs.
- **USB Audio:** Work in progress

## Examples currently present

These examples are present in the repository. Their board-specific pin and codec settings are defined by the project configuration; presence in the tree alone does not imply that every example builds on every target.

| Example | Description |
| --- | --- |
| [AAC Player](examples/AAC_Player/AAC_Player.cpp) | Plays an AAC file from SD_MMC through I2S; reports playback, audio-block and processor statistics. |
| [MP3 Player](examples/MP3_Player/MP3_Player.cpp) | Plays an MP3 file from SD_MMC through I2S and reports playback/decoder statistics. |
| [Internet Radio](examples/Radio/radio.cpp) | Reads an HTTP/HTTPS radio stream, selects the MP3 or AAC player from the response content type and adjusts the output sample rate when requested. Requires local Wi-Fi credentials in secrets.h. **Note:** the sample uses TLS with certificate verification disabled; production applications should validate the server certificate. |
| [Stereo Pass-through](examples/HardwareTesting/PassThroughStereo.cpp) | Routes the left/right I2S input to the matching output channel and reports received, erroneous and dropped input blocks. |
| [Play Queue Demo](examples/Queues/PlayQueueDemo.cpp) | Generates a sine wave in application code and demonstrates AudioPlayQueue buffer/sample submission, including non-stalling operation. |
| [Record to LittleFS](examples/RecordLittleFS/RecordLittleFS.cpp) | Generates a short 880 Hz sine ping with an envelope and records mono PCM WAV to LittleFS using AudioRecordFile. Requires PSRAM and a mounted/formatted LittleFS partition. |
| [Record to SD](examples/RecordSD/RecordSD.cpp) | Generates the same short sine ping and records mono PCM WAV to an SD card using AudioRecordFile. SD_MMC uses the board-specific 4-bit pin mapping from the project configuration. |
| [Delay effect](examples/Effects/Effect-Delay/Effect_Delay.cpp) | Generates a sine chirp, shapes it with an envelope and mixes several delayed taps to the opposite output channel. |
| [FIR and Biquad filters](examples/Effects/Filter_FIR/Filter_FIR.cpp) | Sends white noise through FIR and low-pass biquad paths to the left and right outputs for comparison. |
| [Guitar synthesis](examples/Synthesis/Guitar/Guitar.cpp) | Uses six Karplus–Strong string generators, mixers and chord data to synthesize strummed chords. |
| [Polyphonic synth music](examples/Synthesis/PlaySynthMusic/main.cpp) | Plays a score stored in memory using 16 waveform voices, envelopes and left/right AudioMixerN instances. |
| [Simple Drum](examples/Synthesis/SimpleDrum/SimpleDrum.cpp) | Triggers four simple-drum synthesizers and mixes them to stereo output. |
| [Wavetable Drum Computer](examples/Synthesis/WavetableDrumComputer/WavetableDrumComputer.cpp) | Plays a four-bar, alternative-rock-style intro before looping an original groove. Uses one-shot wavetable playback with CC0 kick, snare, closed/open hi-hat and two tom samples prepared from [Aspirin DX Soundbank — Fairytale Edition](https://github.com/NeoSoundFonts/Aspirin-DX-Soundbank). Records about 60 seconds as mono PCM WAV to SD (`/DrumComputer.wav`); skips recording if `/DrumComputer.wav` or `/DrumComputer.tmp` already exists. Requires SD wiring, a writable card and PSRAM. |
| [Doppler Helicopter](examples/Effects/DopplerHelicopter/DopplerHelicopter.cpp) | Loads a short mono rotor-loop WAV from SD_MMC in 4-bit mode into a PSRAM `AudioSampleBuffer`, then plays it from `AudioPlayMemory` with looping and Doppler playback-rate control, stereo panning and distance-dependent gain. Requires `/Helicopter.wav` to fit in available PSRAM. |
| [Generative Melody](examples/Synthesis/GenerativeMelody/GenerativeMelody.cpp) | Creates C64-inspired, ever-changing melodies with a repeating motif, chord-aware notes, a bass line, a sustained chord pad and four tempo-synchronised melody echoes. |
| [Deep House Groove](examples/Synthesis/DeepHouse/DeepHouse.cpp) | Generates a 125 BPM four-on-the-floor beat with kick, noise clap, closed/open hi-hats with occasional ghost notes, syncopated filtered bass, offbeat seventh-chord stabs and stereo Freeverb ambience. |
| [Neon Noir](examples/Synthesis/NeonNoir/NeonNoir.cpp) | Original 76 BPM cinematic synth piece with slow detuned pads, pulsing sub-bass, a more audible low mechanical drone synthesized from layered oscillators and pink noise, a louder distant slowly sweeping siren heard through the reverb, a sampled saw-table lead with a slow 2.05–2.75 kHz filter sweep, an airy chorale wavetable derived from the CC0-licensed [New Vintage Dreams](https://github.com/NeoSoundFonts/New-Vintage-Dreams) SF2, tempo-synchronised echoes, stereo Freeverb and per-node CPU diagnostics. This checked-in sketch targets the ESP32 ES8311/ES8388 setup. Only compact 256-point tables are embedded; the SF2 file itself is not bundled. |



## Implemented filters

| Class | Functionality and notes |
| --- | --- |
| **AudioFilterBiquad** | Multi-stage biquad filter. Supports low-pass, high-pass, band-pass, notch, peaking EQ, low-shelf and high-shelf coefficient setup. `setPeaking(stage, frequency, gain, q)` controls a bell-shaped frequency band; gain is in dB and Q controls bandwidth. Up to four stages; coefficient handling uses Q30 internally. |
| **AudioFilterFIR** | FIR filter with caller-provided coefficients; supports up to 200 coefficients/taps. |
| **AudioFilterLadder** | Nonlinear ladder low-pass filter based on the Huovilainen/New Moog model. Offers linear or polyphase-FIR interpolation and uses 4× internal oversampling. |
| **AudioFilterStateVariable** | State-variable filter providing low-pass, band-pass and high-pass outputs; also supports a control input for variable cutoff. |

## Implemented effects

| Class | Functionality and notes |
| --- | --- |
| **AudioEffectBitcrusher** | Reduces bit depth and/or effective sample rate. |
| **AudioEffectChorus** | Multi-voice chorus. The application supplies and owns the delay-line memory. |
| **AudioEffectDigitalCombine** | Combines two inputs using OR, XOR, AND, modulo, addition or subtraction. ADD and SUBTRACT saturate to the signed 16-bit PCM range. |
| **AudioEffectDelay** | Delay effect with up to eight delay taps. Its sample ring buffer is allocated lazily, prefers PSRAM and falls back to internal RAM. The default maximum delay is 4000 ms. |
| **AudioEffectEnvelope** | Delay/attack/hold/decay/sustain/release envelope with note-on/note-off control. |
| **AudioEffectFade** | Fade-in and fade-out over a specified duration. |
| **AudioEffectFlange** | Flanging effect; the application supplies the delay-line memory. |
| **AudioEffectFreeverb** | Mono Freeverb reverb. |
| **AudioEffectFreeverbStereo** | Stereo Freeverb variant sharing the ESP32-adapted implementation. |
| **AudioEffectGranular** | Granular freeze and pitch-shift modes using an application-owned sample buffer. |
| **AudioEffectMidSide** | Encodes stereo left/right to mid/side and decodes mid/side back to left/right. |
| **AudioEffectMultiply** | Multiplies two input signals sample by sample. |
| **AudioEffectRectifier** | Full-wave rectification. |
| **AudioEffectReverb** | Reverb with configurable approximate decay time; delay-buffer allocation can use PSRAM. |
| **AudioEffectWaveFolder** | Wave-folding effect with two input connections. |
| **AudioEffectWaveshaper** | Applies a caller-defined transfer curve. The curve is copied into effect-owned storage. |

## Mixers and gain

| Class | Functionality and notes |
| --- | --- |
| **AudioMixer4** | Familiar four-input mixer with per-input gain control. |
| **AudioMixerN** | Extended mixer supporting 4–32 inputs. Accumulates in float and saturates once when converting the final output to 16-bit PCM. |
| **AudioAmplifier** | Single-input gain stage. |


## Audio inputs and outputs

| Class | Functionality and notes |
| --- | --- |
| **AudioInputI2S** | Stereo I2S input using the ESP-IDF I2S driver. Pin assignments are supplied through the Pins configuration. Exposes RX block, error and dropped-block counters. |
| **AudioInputI2S2** | Convenience wrapper selecting the second I2S controller. Board-specific pins must be supplied. |
| **AudioInputI2S3** | Convenience wrapper selecting the third I2S controller, where supported by the SoC. |
| **AudioOutputI2S** | I2S master output using the ESP-IDF driver. Supports explicit BCLK/WS/DOUT/MCLK pins and stereo-to-mono output selection. |
| **AudioOutputI2S2** | Convenience wrapper selecting the second I2S controller. Board-specific pins must be supplied. |
| **AudioOutputI2S3** | Convenience wrapper selecting the third I2S controller, where supported by the SoC. |
| **AudioOutputPDM** | Hardware PCM-to-PDM DAC output on I2S0; supports mono or stereo data lines on compatible targets. |
| **AudioOutputSPDIF** | Software IEC 60958 consumer S/PDIF output using a fixed BMC lookup table and I2S0. Requires an optical transmitter or a suitable coaxial line driver; do not connect GPIO directly to coax. |
| **AudioOutputSPDIF2** | Same software S/PDIF encoder routed through I2S controller 1; availability and pins depend on the SoC and board. |
| **AudioOutputSPDIF3** | Same software S/PDIF encoder routed through I2S controller 2; available only on SoCs that provide a third I2S controller. |
| **AudioRecordFile** | Stand-alone mono/stereo 16-bit PCM WAV recorder. Uses a PSRAM ring buffer (512 KiB by default) and a separate writer task, optionally pinned to a core. WAV sizes are finalized and the temporary file renamed on successful stop. |


The I2S2/I2S3 wrappers do not add extra hardware: they select a controller index. Whether those controllers exist, can be used concurrently and expose suitable pins depends on the particular ESP32 variant and board. AudioControlES8311 and AudioControlES8388 are available as codec-control classes, not as audio-stream inputs or outputs themselves. PDM direct-DAC output requires a compatible SoC/ESP-IDF combination; see [I2S.md](lib/I2S.md) for pin mapping and clock configuration.

USB Audio input/output remains WIP. S/PDIF output variants `AudioOutputSPDIF`, `AudioOutputSPDIF2` and `AudioOutputSPDIF3` are exposed through `Audio.h`. Each output requires exclusive use of its selected I2S controller; physical controller availability varies by SoC.
TDM is planned; PDM output is implemented on supported targets.

## Players and audio sources

| Class | Functionality and notes |
| --- | --- |
| **AudioPlayMp3** | MP3 decoder/player. Can play from an Arduino filesystem or a generic AudioSource; used by the radio example for MP3 streams. |
| **AudioPlayAac** | AAC decoder/player. Can play from an Arduino filesystem or a generic AudioSource; used by the radio example for AAC streams. |
| **AudioPlaySdWav** | WAV/PCM file player for filesystems and seekable AudioSource objects. Supports loop and playback-rate control, and can preload a supported PCM WAV into a mono PSRAM AudioSampleBuffer for AudioPlayMemory. |
| **AudioPlayMemory** | Plays legacy uncompressed PCM arrays from wav2sketch or mono PCM from AudioSampleBuffer without copying; μ-law-encoded legacy arrays are not decoded. Supports looping and variable playback rate. The buffer must outlive playback. |
| **AudioPlayQueue** | Supplies application-generated samples or blocks into the audio graph; includes ORIGINAL and NON_STALLING queue behaviours. |
| **AudioPlayer** | Common player interface used by the decoder/file-player architecture, with codec metadata, MIME matching, playback state, position and duration methods. It is an abstract interface, not a concrete decoder. |

MP3 and AAC use the ESP-beatsy software-decoder/AudioDecoderStream path. The radio example currently selects between those two formats based on the HTTP content type. Although the AudioCodec enumeration also contains WAV and Opus values, **there is no AudioPlayOpus implementation in the current inventory**; an enum value does not mean the corresponding player is implemented.

## Synthesizers

| Class | Functionality |
| --- | --- |
| **AudioSynthWaveformDc** | Constant/DC signal source. |
| **AudioSynthWaveformSine** | Sine oscillator. |
| **AudioSynthWaveformSineHires** | High-resolution sine oscillator variant. |
| **AudioSynthWaveform** | General waveform oscillator. |
| **AudioSynthWaveformModulated** | Waveform oscillator with modulation input. |
| **AudioSynthWaveformPWM** | PWM-style waveform generator inside the audio graph. This is a PCM audio source, not a hardware GPIO PWM output class. |
| **AudioSynthToneSweep** | Swept-frequency tone generator. |
| **AudioSynthNoiseWhite** | White-noise generator. |
| **AudioSynthNoisePink** | Pink-noise generator. |
| **AudioSynthWavetable** | Wavetable-based synthesis. |
| **AudioSynthSimpleDrum** | Simple percussive/drum synthesis. |
| **AudioSynthKarplusStrong** | Plucked-string synthesis using the Karplus–Strong algorithm. |

A 513 entry sine table replaces the Teensy 257 entry table.

## Audio analysis

- **AudioAnalyzeFFT256** — 256-point FFT analysis.
- **AudioAnalyzeFFT1024** — 1024-point FFT analysis.
- **AudioAnalyzePeak** — peak-level measurement.
- **AudioAnalyzeRMS** — RMS-level measurement.

## Build and board configuration

Use the PlatformIO environments in [platformio.ini](platformio.ini) as the starting point for the intended board and framework configuration. Before running an example, check its pin definitions and codec selection (ES8311 or ES8388 where applicable). Network examples also need local credentials/configuration.

All SD-card examples use `SD_MMC` in **4-bit mode**. The ESP32-A1S Audio Kit and Freenove ESP32-S3 Display pin mappings (`CLK`, `CMD`, `D0`–`D3`) are defined in `platformio.ini`; the old SPI SD pin definitions have been removed. On the ESP32-A1S Audio Kit, set the SD-card switches `CMD` and `DATA3` to ON.

For board-specific requirements, implementation details and buffer/task notes, see the [library technical overview](lib/AudioLibrary/README.md). The current implementation is evolving, so treat the source in the selected branch as authoritative when an API or hardware detail matters.
