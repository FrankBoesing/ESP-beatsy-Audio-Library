# ESP-beatsy Audio System Design Tool

This directory contains the offline Audio System Design Tool, adapted from the
Teensy Audio System Design Tool for the ESP-beatsy Audio Library.

- Start: open `index.html` in a modern browser.
- Node palette: includes ESP-beatsy classes for I2S controllers, S/PDIF, PDM,
  ES8311/ES8388 codec control, MP3/AAC playback, AudioMixerN and WAV recording.
- Export: generates the Audio graph declarations and `AudioConnection` lines
  as a starting point for an Arduino/PlatformIO sketch.
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
