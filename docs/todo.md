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
