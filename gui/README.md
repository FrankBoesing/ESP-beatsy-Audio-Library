# ESP-beatsy Audio System Design Tool

This directory contains the offline Audio System Design Tool, adapted from the
Teensy Audio System Design Tool for the ESP-beatsy Audio Library.

- Start: open `index.html` in a modern browser.
- Node palette: includes ESP-beatsy classes for I2S controllers, S/PDIF, PDM,
  ES8311/ES8388 codec control, MP3/AAC playback, AudioMixerN and WAV recording.
- Export: generates the Audio graph declarations and `AudioConnection` lines
  as a starting point for an Arduino/PlatformIO sketch.
- Biquad Calculator: standalone offline tool for low-pass, high-pass, band-pass, notch and shelf filters. It plots the response and exports normalized Q30 coefficients and `AudioFilterBiquad` setup code.
- EQ Chain Designer: designs and plots up to four cascaded `AudioFilterBiquad` stages and exports the corresponding setup calls. It uses the filter types currently implemented by the library; there is no dedicated peaking-EQ method.
- FIR Calculator: standalone offline tool for windowed-sinc low-pass, high-pass,
  band-pass and band-stop design. It plots the quantized response and exports
  signed 16-bit Q15 coefficients for `AudioFilterFIR` (up to 199 odd-numbered taps).
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
