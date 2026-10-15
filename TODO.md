# TODO

## Audio filter tools

- [ ] **Make sample-rate handling consistent in the Biquad Calculator and EQ Chain Designer.** Ensure the selected sample rate matches the library's compile-time `AUDIO_SAMPLE_RATE_EXACT`, or clearly validate/report mismatches before exporting setter calls.
- [ ] **Implement a peaking-EQ filter in `AudioFilterBiquad`.** Add the API and coefficient calculation, then expose it in the Biquad Calculator and EQ Chain Designer and verify its frequency response.
