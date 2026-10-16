# Hybrid handpan synthesizer

`AudioSynthHandpan` combines a short caller-supplied mono PCM16 attack with a
fixed bank of damped resonators. Three longer body modes model the note body;
three optional short-decay modes add a metallic strike. The resonators are
independent per configured pad. A repeated strike adds energy to the existing
resonator states, so a pad is not cut off when it is struck again.

## Limits and runtime behavior

- Up to 16 configured pad slots, six modes per pad and eight concurrent attack
  sample voices.
- A fixed 16-entry event ring buffer accepts note events without dynamic
  allocation. Its usable capacity is 15 queued events; `noteOn()` returns false
  for an unconfigured pad or a full queue.
- Velocity is 0..127 and scales both the attack sample and each resonator mode.
- The class produces mono PCM16. Connect its output to both I2S channels or to
  a mixer as needed.
- Decay is specified as `T60`, the time for modal amplitude to fall by 60 dB.
- The sample pointer is retained, not copied. Keep its data alive and unchanged
  during playback; `const` samples mapped from flash are suitable.
- Configure the pad bank before starting audio updates (normally in `setup()`
  before `AudioMemory()`). Configure again after changing the global sample rate.
- If `noteOn()` returns false, the event was not queued and should be retried or
  counted by the application. Calls are block-scheduled by the current audio
  graph, so the onset resolution is one audio block, not per-sample MIDI timing.

## API

```cpp
AudioSynthHandpan handpan;

AudioSynthHandpan::ModeConfig modes[6] = {
    // frequency Hz, peak gain, T60 seconds, phase radians
    {146.8f, 0.315f, 2.00f, 0.12f},
    {293.6f, 0.045f, 1.05f, 1.10f},
    {440.4f, 0.135f, 0.58f, 2.05f},
    {933.0f, 0.055f, 0.075f, 0.90f},
    {1300.0f, 0.0352f, 0.115f, 2.15f},
    {1710.0f, 0.0231f, 0.095f, 0.25f},
};

// attack is a caller-owned signed mono PCM16 sample; nullptr disables it.
handpan.configureNote(0, modes, 6, attack, attackSampleCount, 44100, 0.09f);
handpan.noteOn(0, 100);
```

`ModeConfig::gain` is a normalized peak amplitude per mode (0..1), not a
percentage. `attackGain` is an additional normalized linear gain applied to the
sample. The initial settings from the Python prototype use an attack gain of
`0.09`, three body modes with `T60 = 2.00, 1.05, 0.58 s`, and three metallic
modes with ratios `6.35, 8.85, 11.65` times the pad's base frequency. The
metallic modes use `T60 = 0.075, 0.115, 0.095 s` and gains `0.055, 0.0352,
0.0231` respectively. These are starting parameters from a listening prototype,
not physical measurements of every real handpan.

## Example and recordings

Open `examples/Synthesis/Handpan/Handpan.cpp`. It demonstrates overlapping
notes, simultaneous strikes and CPU/audio-block diagnostics. To keep the
repository free of recordings with unverified redistribution terms, the example
generates a short procedural transient at startup. Replace it with an approved
recording converted to a `const int16_t[] ` table for the reference-derived
hybrid timbre. Do not redistribute extracted reference samples unless their
license explicitly permits it.

## Performance notes

`update()` performs no trigonometric or exponential calculations; modal
coefficients are prepared in `configureNote()`. Active resonators and active
attack voices are processed, and silent resonators are disabled once their
state falls below the output threshold. If the audio block pool is temporarily
exhausted, oscillator states and attack positions still advance to preserve
wall-clock decay. Actual CPU cost must be measured on the target, particularly
with all 16 pads ringing and eight attack voices active; the included example
prints `AudioProcessorUsage()` and audio memory usage on the device.


## Reverb and worst-case load test

The Handpan example routes the source to a dry path and to mono
`AudioEffectFreeverb`, then blends them through `AudioMixer4`. The starting
mix is 0.82 dry / 0.22 wet, room size 0.68 and damping 0.46. These are
listening defaults, not calibrated acoustic-room parameters. Freeverb allocates
its delay lines once in `setup()`; the example requests 32 audio blocks to cover
the source, reverb return and final mixer.

The example cycles through three eight-second phases:

1. `MELODY + HALL`: a moderate overlapping melody.
2. `ALL 16 PADS / 96 MODES`: each pad is struck in turn, about once per second,
   so its long body resonances overlap with the tails of the other pads.
3. `RAPID 4-NOTE BURSTS`: four pad events every 24 ms, stressing the fixed event
   queue and the eight attack-sample voice limit while modal tails remain active.

Once per second the serial log reports graph-wide CPU, per-stream CPU for the
handpan source and Freeverb, current/peak audio-block use, and note events
rejected by the synthesizer. Let each phase run for several seconds when
comparing boards. The measured maximum is specific to the test run; it is not
a guarantee for every sample rate, compiler optimization, Wi-Fi load or effect
chain. Listen for clipping or roughness during the intentionally extreme stress
phase as well as checking CPU headroom.

The example generates its own short procedural attack. A licensed PCM16 attack
sample can be substituted without changing the synth architecture.
