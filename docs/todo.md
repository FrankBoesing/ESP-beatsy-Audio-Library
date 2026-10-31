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

## SF2 SoundFont Import / AudioSynthWavetable

The planned SF2 tool should generate native `AudioSynthWavetable` instrument
headers, not just extract WAV files. This requires a small, backwards-compatible
extension of the wavetable engine before the converter can export complete
presets.

- [ ] **Extend `AudioSynthWavetable` to represent SF2 sample zones.** The
  current `instrument_data` model uses sorted `sample_note_ranges` and selects
  exactly one sample using the note number. It cannot express SF2 key *and*
  velocity rectangles, preset/instrument global and local zone combinations,
  or overlapping zones that layer multiple samples. Add a zone-based instrument
  representation with key-low/high and velocity-low/high bounds, and resolve
  all matching zones at note-on. Support a bounded number of simultaneous
  sample layers per note, with independent sample phase, loop and envelope
  state per layer; mix them safely without heap allocation in `update()`.
  Keep the existing `instrument_data` layout and current API working for
  existing generated instruments and examples.
- [ ] **Add explicit velocity-zone selection without breaking amplitude
  control.** The existing `playNote(note, amp)` argument controls note level
  and is not currently used to select a zone. Define an unambiguous velocity
  input/path (and retain a separate output-level control if needed), then use
  MIDI velocity to select/intersect SF2 velocity ranges. Define how overlapping
  velocity layers are mixed and how the maximum layer count is enforced.
- [ ] **Complete the SF2 loop behaviour needed by the engine.** Map forward
  sample loops to `LOOP_PHASE_END` and `LOOP_PHASE_LENGTH`. Add a loop mode
  that can leave the loop on note-off for SF2 loop-until-release zones; the
  current `LOOP` flag wraps continuously even while the release envelope is
  running. Define and document the handling of unsupported ping-pong/reverse
  loops. Preserve the existing one-shot and continuous-loop behaviour.
- [ ] **Map supported SF2 generators to `sample_data`.** Convert root key,
  coarse/fine tuning and sample rate into the phase increment; convert sample
  start/end and loop offsets into phase limits; translate volume-envelope
  timecents and sustain centibels into delay/attack/hold/decay/release counts
  and `SUSTAIN_MULT`; map initial attenuation and supported vibrato/modulation
  parameters to the existing fields. Verify the time and phase calculations at
  all runtime sample rates supported by the library.
- [ ] **Define explicit handling for SF2 features the current node cannot
  render.** `AudioSynthWavetable` is currently a mono output with one
  `int16_t` sample stream per layer. The first converter version should
  downmix linked stereo sample pairs to mono and convert optional SF2 24-bit
  `sm24` data to the engine's 16-bit PCM format. Report unsupported filter
  generators, arbitrary modulators, stereo placement and effects instead of
  silently implying that they are reproduced. Keep those features as possible
  later extensions, not prerequisites for the first usable converter.
- [ ] **Adapt the planned SF2 browser tool to the new instrument model.** Keep
  local/offline file processing, but change its main output from raw sample WAV
  export to a compile-ready C++ header for a selected SF2 preset. The header
  must contain deduplicated PCM sample arrays, zone descriptors with key and
  velocity ranges, per-zone `sample_data` parameters and the instrument
  descriptor expected by the extended API. Preserve linked stereo samples by
  the documented mono-downmix policy and convert `sm24` data to 16-bit PCM.
  Correctly combine preset and instrument global/local generators before
  calculating each zone. Preview should use the same note/velocity zone
  selection and layer-mixing rules as the exported instrument. Keep optional
  per-sample WAV export only as a diagnostic feature, not as the primary result.
- [ ] **Validate SF2 conversion and playback end-to-end.** Check preset/bank
  selection, key/velocity boundaries, overlapping layers, root-key tuning,
  loops including note-off, envelope timing, sample-rate adaptation, memory
  footprint and clipping. Compare browser preview with the generated
  `AudioSynthWavetable` output on ESP32-S3 and ESP32-P4. Include at least one
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
