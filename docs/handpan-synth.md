# Elisa's Hand Pan — hybrid handpan synthesizer

`AudioSynthHandpan` combines a short finger-contact transient with damped
resonators that provide the sustained handpan-like tone. The musical example is
`examples/Synthesis/ElisasHandPan/ElisasHandPan.cpp`; it plays the opening
30 seconds of Beethoven's public-domain **Für Elise (WoO 59)**, based on the
upper-staff note sequence and onset timing from the
[Mutopia Project score](https://www.mutopiaproject.org/cgibin/piece-info.cgi?id=931).
The isolated E6 at 25.833 seconds is lowered by one octave to E5.

## Simple use

The library generates the default attack itself. A sketch needs no embedded
PCM table, sample-generation function or external recording.

```cpp
AudioSynthHandpan handpan;

AudioSynthHandpan::ModeConfig modes[3] = {
    // frequency Hz, normalized gain, T60 seconds, initial phase radians
    {261.626f, 0.21f, 3.00f, 0.0f},
    {525.25f,  0.06f, 3.00f, 0.0f},
    {780.9f,   0.16f, 4.00f, 0.0f},
};

// Set I2S sample rate before configuring notes.
handpan.configureNote(0, modes, 3);  // built-in soft attack
handpan.noteOn(0, 90);
```

Configure pads before starting audio updates (normally before `AudioMemory()`)
and after selecting the output sample rate. `noteOn()` accepts a pad slot from
0 to 15 and a MIDI-like velocity from 1 to 127. It returns `false` if the pad
is not configured or the fixed event queue is full. The event queue can hold
15 pending events.

## Built-in attack

The three-argument `configureNote(slot, modes, modeCount)` overload enables a
30 ms transient generated once in library-owned RAM. It is shared by all
`AudioSynthHandpan` instances and pads, and occupies 2,646 bytes. The waveform
combines low-passed pseudo-random noise with two quiet low-frequency components;
a 2.5 ms smooth rise and 4 ms tail fade avoid a sharp click. Its source peak is
normalized to **0.36 full scale** and the default playback gain is **0.10**.

A custom signed mono PCM16 sample can still be passed through the longer
`configureNote()` overload. Its pointer is retained, not copied, so the buffer
must remain valid and unchanged during playback. Pass `nullptr` and a sample
count of zero to select resonator-only playback with that overload. Custom
samples must be at least two samples long and no longer than
`MAX_ATTACK_SAMPLES`.

## Modal synthesis and onset

Each pad can have up to six modes. The Für Elise example uses three body modes;
the unused upper partials were removed from the sketch because their gains were
zero and they did not contribute to the sound.

A mode uses the recurrence

`y[n] = 2 r cos(ω) y[n-1] - r² y[n-2]`

with `ω = 2π f / fs`, where `f` is its frequency and `fs` is the sample rate.
For a requested amplitude decay time `T60`, the coefficient is

`r = 10^(-3 / (T60 × fs))`.

`T60` is the time required for the modal amplitude to decay by 60 dB. The
coefficients are calculated during `configureNote()`, not in the audio-block
render loop. Trailing modes with zero gain are now excluded from runtime mode
loops, reducing work without changing the output; gaps before a later active
mode remain supported.

Each strike starts in an independent resonator voice and fades in with a
5 ms smoothstep ramp. When the ramp has finished, that voice's state is merged
into the pad's persistent resonators at the end of an audio block. Existing
resonator tails are not reset or ducked, so repeated strikes add energy rather
than abruptly cutting off the previous ring. The implementation supports up to
32 short-lived strike voices and eight overlapping PCM attack voices.

The source produces mono PCM16. The example sends it to both I2S output
channels, with a shared Freeverb return mixed alongside the dry signal.
Saturation is optional and can be bypassed with `setSaturation(0.0f)`.

## Example details

`ElisasHandPan.cpp` uses the library's default 44.1 kHz sample rate. A reference
recording of the synthesized phrase is available as
[ElisasHandPan.mp3](../examples/Synthesis/ElisasHandPan/ElisasHandPan.mp3).

The excerpt contains 109 note events and 12 distinct pitches: C4, E4, F4, G4,
G#4, A4, B4, C5, D5, D#5, E5 and F5. Note onset times are preserved, except
that the isolated E6 at 25.833 seconds is played as E5. Resonators ring
according to their decay settings rather than following piano note-off
envelopes exactly.



The frequency, gain and decay values are experimental synthesis parameters,
not measured modes of a specific commercial handpan. Listen on the target
hardware and check CPU/audio-block headroom when changing the number of modes
or adding other effects.
