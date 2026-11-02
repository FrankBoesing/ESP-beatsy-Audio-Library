# TODO

This file lists planned work that is not yet implemented. Existing I/O nodes and
the audio recorder are intentionally omitted because they are already present.

## USB Audio

- [ ] **Explore USB multichannel capture beyond stereo.** Extend Arduino-ESP32
  `USBAudioCard` channel configuration, UAC descriptors and TinyUSB
  endpoint/FIFO settings. Measure the practical maximum for the ESP32-S3
  Full-Speed USB bus, memory and CPU, and validate host compatibility. This is
  not a CPU-only limit.
- [ ] **Add optional sample-rate conversion to `AudioOutputUSB`.** Keep the
  USB/UAC rate selected by the host independent of the radio decoder's
  44.1/48 kHz source rate. Use a stateful resampler with anti-alias filtering
  and verify audio quality, CPU load, latency and long-duration buffer
  stability. Defer until the remaining USB Audio path is stable.

## Audio Filter Tools

- [ ] **Make sample-rate handling consistent in the Biquad Calculator and EQ
  Chain Designer.** Ensure the selected sample rate matches the library's
  compile-time `AUDIO_SAMPLE_RATE_EXACT`, or clearly validate and report
  mismatches before exporting setter calls.
- [ ] **Add a Linkwitz-Riley crossover designer (GUI).** Support two-way and
  three-way systems, selectable crossover frequencies and slopes (at least
  LR2/LR4), combined response plots, and generated `AudioFilterBiquad`
  setup code. Reuse existing biquad stages initially; a new DSP class is not
  required.

## SF2 SoundFont Import / AudioSynthWavetableV2

The SF2 tool will target the new, independent `AudioSynthWavetableV2` class.
The legacy `AudioSynthWavetable` class, its data layout and its API remain
unchanged so existing sketches and generated headers continue to work.

- [x] **Add the initial polyphonic `AudioSynthWavetableV2` engine.** The new
  class has a fixed pool for 32 MIDI notes and up to four sample layers per
  note (128 sample-playback instances total), matches key/velocity zones,
  keeps independent phase and volume-envelope state per layer, supports
  note-on/note-off, 16 MIDI channels, sustain pedal, one-shot/continuous/
  loop-until-release playback and oldest-note voice stealing. Voice stealing is
  currently immediate; click-free stealing/crossfades remain a later task.
  Mixing uses a single 32-bit accumulator and saturates once at the output. No heap
  allocation occurs in `update()`. V2 supports mono PCM and optional linked
  left/right signed 16-bit PCM per sample layer. Stereo layers share phase,
  pitch, loop and envelope state; the two channels use independent interpolation
  and mix accumulators. Mono layers remain on the mono kernel and are mirrored
  to both outputs when any stereo layer is active. Each channel array includes
  one interpolation guard frame.
- [x] **Add a configurable hardware stress-test example for V2.**
  `examples/Synthesis/WavetableV2` routes output indices 0/1 to the two
  channels of `AudioOutputUSB` and reports total audio-graph CPU usage,
  V2-node CPU usage, peak load and audio-block memory usage. Define
  `WAVETABLE_V2_TEST_STEREO=1` to replace one mono layer with a linked
  left/right sample and exercise mixed mono/stereo rendering. The test defaults
  to one MIDI note with four overlapping sample layers; set
  `WAVETABLE_V2_TEST_NOTE_COUNT` in build flags to increase the load up to
  32 notes / 128 sample layers. The host PC can record the stereo 16-bit USB
  microphone stream with Audacity or another audio recorder.
- [x] **Apply the first portable scalar optimization pass to V2.** The renderer
  now snapshots and commits voices in batches of eight (with the generation
  check retained), retires finished notes with one active-voice scan, advances
  envelopes inline with saturating 32-bit arithmetic, hoists loop/phase
  invariants out of the sample loop, and uses a Q16 effective envelope gain
  plus 32-bit interpolation/mixing multiplies instead of per-sample 64-bit
  multiplies.
- [ ] **Benchmark and continue optimizing the V2 render and mix hot paths.**
  Measure the changes on real ESP32-S3 and ESP32-P4 hardware against the
  pre-optimization baseline. Check audio continuity, envelope/release behavior,
  interpolation error and worst-case block time at 4, 8, 16, 32, 64 and 128
  sample voices. Keep a portable scalar implementation as the reference/
  fallback. Evaluate separately compiled, compile-time-selected optimized
  kernels for ESP32-S3 (Xtensa LX7 DSP/SIMD capabilities) and ESP32-P4
  (RISC-V core and its verified compiler/intrinsic capabilities). Use
  target-specific code only where repeatable benchmarks show a measurable
  benefit; avoid unverified instruction assumptions.
- [ ] **Benchmark V2 polyphony and memory placement.** Measure 16, 32, 64 and
  128 simultaneous sample instances at 44.1 and 48 kHz on ESP32-S3 and
  ESP32-P4. Record V2-node and total graph CPU usage, peak block time, audio
  memory use, clipping and underruns. Compare internal SRAM and PSRAM sample
  data, aligned and unaligned access, and realistic SF2 sample lengths. Keep
  benchmark results by target and toolchain version.
- [ ] **Add per-layer vibrato / pitch modulation.** Implement SF2-compatible
  vibrato delay, rate and depth with independent state per sample layer.
  Verify pitch range, modulation update cost and stable behavior when notes
  are released or voices are stolen. Add additional supported modulators only
  after the baseline vibrato path is measured.
- [x] **Add stereo sample playback.** A sample layer may reference optional
  linked left/right PCM arrays while sharing phase, pitch, loop and envelope
  state. Stereo uses output indices 0/1; mono samples are mirrored to both
  channels without entering the stereo sample loop. If the audio block pool
  cannot provide the second output block, the current update falls back to a
  mono average for that block. SF2 pan generators and arbitrary stereo
  placement are not applied yet. The legacy V1 converter remains mono and
  deliberately downmixes linked stereo pairs; the V2 exporter now preserves
  linked left/right sample arrays.
- [x] **Add key/velocity zone selection.** V2 resolves all matching
  key-low/high and velocity-low/high rectangles at note-on. Velocity selects
  the zones and also sets the initial note level; `amplitude()` is a separate
  master control that affects already-playing notes. The current hard limit is
  four matching sample layers per note; additional matching zones are ignored
  after the first four.
- [x] **Add SF2 forward-loop modes.** V2 supports one-shot, continuous looping
  and loop-until-release. Ping-pong and reverse loops remain unsupported and
  must be reported by the converter.

- [x] **Map the core SF2 playback generators to V2 `sample_data`.** The
  converter maps root key, coarse/fine tuning, pitch correction and each
  sample's native sample rate into the phase coefficient; maps sample start/end
  and forward-loop offsets into phase limits; converts volume-envelope
  timecents and sustain centibels into delay/attack/hold/decay/release counts
  and `SUSTAIN_MULT`; and maps initial attenuation. When fixed key number,
  non-100-cent scale tuning, or key-tracked hold/decay changes the values by
  note, the exporter splits that SF2 key range into single-key descriptors.
  Runtime sample-rate adaptation is handled by V2 relative to
  `AUDIO_SAMPLE_RATE_EXACT`. Per-layer vibrato and other modulation remain a
  separate TODO; unsupported generators are reported.
- [x] **Define explicit handling for SF2 features V2 cannot render.**
  The V2 exporter preserves mono PCM and linked left/right `int16_t` PCM
  streams, including interpolation guard frames, and rounds optional `sm24`
  low-byte data to 16-bit PCM. It reports unsupported generators, fixed-velocity
  overrides, modulators, filters, pan/placement, effects sends, invalid forward
  loops and the reserved sample mode instead of implying that they are
  reproduced. Legacy V1 export continues to downmix linked stereo pairs.
- [x] **Create V1 of the offline SF2 converter for the legacy wavetable API.**
  Implemented in `docs/tools.html` and `docs/sf2-tool.js`: local SF2 parsing,
  preset/instrument global-local zone resolution, fixed-velocity export,
  single-sample-per-key header generation, note audition with browser-side
  layering, linked-stereo downmix and 24-to-16-bit conversion. V1 deliberately
  rejects overlapping key layers and samples longer than 65,535 frames instead
  of producing incomplete instrument data. This does not complete the target
  feature below; the converter must be upgraded after the `AudioSynthWavetable`
  zone/layer API exists.
- [x] **Add an interactive SF2 keyboard player.** The browser tool now provides
  a two-octave keyboard for mouse/touch input and the PC keyboard, including
  held-note Note On/Off and volume-envelope release. QWERTZ/QWERTY labels are
  read from the browser keyboard map when available, or detected from the
  actual Y/Z key event (also suitable for Firefox). Its default
  compatibility mode is monophonic, requires exactly one matching sample zone
  per note at the selected velocity, applies the current default wavetable
  amplitude and enforces the legacy 65,535-frame sample limit. An optional
  browser mode allows up to 16 simultaneous notes and eight layers per note.
  Browser sample buffers are bounded to avoid unbounded memory growth. This is
  preview functionality only; it does not implement missing SF2 modulators,
  filters, pan or effects in the ESP32 synth.
- [x] **Upgrade the SF2 browser tool to the V2 instrument model.** The offline
  tool now defaults to a compile-ready `AudioSynthWavetableV2` header and
  keeps an explicit legacy V1 export option. V2 export preserves resolved
  key/velocity rectangles, overlapping layers (with a full-domain check
  against the engine's four-layer limit), deduplicated mono or linked stereo
  PCM arrays, per-zone sample descriptors, supported envelope/tuning/loop
  parameters and `sm24` conversion. Key-specific descriptors are emitted
  where SF2 key-number, scale-tuning or key-tracked envelope settings require
  them. The browser preview can play linked stereo and has a V2-compatible
  32-note/four-layer mode. Unsupported features are described in the generated
  header and preset report.
- [ ] **Validate SF2 conversion and playback end-to-end.** Check preset/bank
  selection, key/velocity boundaries, overlapping layers, root-key tuning,
  loops including note-off, envelope timing, sample-rate adaptation, memory
  footprint and clipping. Compare browser preview with the generated
  `AudioSynthWavetableV2` output on ESP32-S3 and ESP32-P4. Include at least one
  simple single-zone preset and one preset with velocity splits and layered
  zones in the tests.

## Planned DSP Filters

Keep this priority order:

1. [ ] **Comb filter (`AudioFilterComb`).** Add feedforward and/or feedback
   comb filtering with configurable delay, feedback amount, damping and
   dry/wet mix. Define safe feedback limits and test stability, clipping and
   state reset when delay settings change.
2. [ ] **TPT state-variable filter.** Add an alternative
   topology-preserving-transform (TPT) SVF rather than replacing the existing
   `AudioFilterStateVariable`. Provide low-pass, band-pass and high-pass
   outputs, resonance control and cutoff modulation. Verify stability and
   response during fast cutoff sweeps.
3. [ ] **Formant / vowel filter (`AudioFilterFormant`).** Build a
   vocal-character filter from several resonant bands, with useful vowel
   presets (A/E/I/O/U) and smooth morphing between presets. Validate resonance
   frequencies, bandwidths, output headroom and interpolation behavior.

## Additional Audio I/O Drivers

The basic I2S, I2S2/I2S3, PDM output and SPDIF output implementations already
exist and are not repeated here. The following variants from the older TODO
list are still missing and should be evaluated against the actual ESP32
hardware capabilities before implementation.

### Inputs

- [ ] Multichannel I2S: `input_i2s_quad`, `input_i2s_hex`,
  `input_i2s_oct`.
- [ ] TDM: `input_tdm`, `input_tdm2`.
- [ ] PDM: `input_pdm`, `input_pdm_i2s2`.

### Outputs

- [ ] Multichannel I2S: `output_i2s_quad`, `output_i2s_hex`,
  `output_i2s_oct`.
- [ ] PT8211: `output_pt8211`, `output_pt8211_2`.
- [ ] TDM: `output_tdm`, `output_tdm2`.
- [ ] ADAT: `output_adat`.

## Additional Codec Controls

These drivers were listed in the older TODO file but are not currently
implemented. Add them only when there is a concrete hardware requirement.

- [ ] SGTL5000: `control_sgtl5000`.
- [ ] WM8731: `control_wm8731`.
- [ ] AK4558: `control_ak4558`.
- [ ] CS4272: `control_cs4272`.
- [ ] CS42448: `control_cs42448`.
- [ ] TLV320AIC3206: `control_tlv320aic3206`.

## Ideas Requiring Specification

- [ ] **Tube coloration (Röhrenklang).** This item appeared in the older TODO
  file without a technical description. Define the intended sound, controls
  and implementation approach before scheduling development.
