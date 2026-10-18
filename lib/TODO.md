# TODO

## Audio filter tools

- [ ] **Make sample-rate handling consistent in the Biquad Calculator and EQ Chain Designer.** Ensure the selected sample rate matches the library's compile-time `AUDIO_SAMPLE_RATE_EXACT`, or clearly validate/report mismatches before exporting setter calls.

## Planned filter enhancements — priority order

1. [ ] **Linkwitz–Riley crossover designer (GUI).** Add a dedicated crossover tool for two-way and three-way systems, with selectable crossover frequency and slope (at least LR2/LR4), combined response plots, and generated `AudioFilterBiquad` setup code. Reuse existing biquad stages; a new DSP class is not initially required.
2. [ ] **Comb filter (`AudioFilterComb`).** Add feedforward and/or feedback comb filtering with configurable delay, feedback amount, damping and dry/wet mix. Define safe feedback limits and test stability, clipping and state reset when delay settings change.
3. [ ] **TPT state-variable filter.** Add an alternative topology-preserving-transform (TPT) SVF rather than replacing the existing `AudioFilterStateVariable`. Provide low-pass, band-pass and high-pass outputs, resonance control and cutoff modulation; verify stability and response during fast cutoff sweeps.
4. [ ] **Formant / vowel filter (`AudioFilterFormant`).** Build a vocal-character filter from several resonant bands, with useful vowel presets (A/E/I/O/U) and smooth morphing between presets. Validate resonance frequencies, bandwidths, output headroom and interpolation behavior.

