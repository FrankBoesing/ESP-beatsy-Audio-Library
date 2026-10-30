# Drum synthesizers

ESP-beatsy provides three sample-free percussion voices. Each is an independent
AudioStream node with one mono output and no audio inputs. The voices are
synthesized in the audio update task; they do not allocate sample buffers
or require external WAV data.

## AudioSynthKick

A compact analog-style bass drum: a sine body is swept down from a higher
starting pitch, while a short noise transient adds beater attack.

| Method | Description |
| --- | --- |
| **noteOn(velocity = 100)** | Trigger/retrigger the kick. Velocity is 0–127; zero is ignored. |
| **frequency(hz)** | Final body frequency. Default: 52 Hz. |
| **length(ms)** | Approximate amplitude decay time to -60 dB. Default: 210 ms. |
| **pitchSweep(octaves)** | Initial pitch rise above the body frequency, in octaves. Range: 0–3. Default: 2. |
| **pitchDecay(ms)** | Exponential time constant of the pitch drop. Default: 27 ms. |
| **click(level)** | Beater-noise amount, 0–1. Default: 0.16. |

## AudioSynthTom

A tuned tom based on three inharmonic circular-membrane modes (frequency
ratios 1.000, 1.593 and 2.136). The higher modes decay faster than the body,
a short filtered-noise burst models the stick impact, and the pitch settles
down from a slightly higher initial frequency.

| Method | Description |
| --- | --- |
| **noteOn(velocity = 100)** | Trigger/retrigger the tom. Velocity is 0–127; zero is ignored. |
| **frequency(hz)** | Fundamental frequency. Default: 125 Hz. |
| **length(ms)** | Approximate amplitude decay time to -60 dB. Default: 420 ms. |
| **pitchSweep(octaves)** | Initial pitch rise in octaves. Range: 0–1.5. Default: 0.26. |
| **pitchDecay(ms)** | Exponential time constant of the pitch drop. Default: 42 ms. |
| **secondMix(level)** | Amount of the two upper membrane modes, 0–1. Default: 0.55. Zero leaves only the fundamental. |
| **attack(level)** | Short filtered-noise stick transient, 0–1. Default: 0.22. Zero disables it. |

The modal ratios approximate the first three modes of an ideal circular
membrane. Their amplitudes and decay times differ, so the sound is not just a
pair of harmonically related sine waves. Frequency limits keep the highest
mode below Nyquist, including at maximum sweep.

## AudioSynthSnare

A snare combines a band-limited noise burst (difference of two low-pass noise
signals) with two damped shell modes. Its noise and shell envelopes have
different decay times. The noise tail is kept close to the shell's decay time,
so the wire rattle fades together with the body instead of disappearing early.
Each object is a single voice: a new noteOn() retriggers and restarts that voice
rather than layering a second copy over the old tail.

| Method | Description |
| --- | --- |
| **noteOn(velocity = 100)** | Trigger/retrigger the snare. Velocity is 0–127; zero is ignored. |
| **frequency(hz)** | Shell fundamental. Default: 185 Hz. The second mode is at 1.78 times this frequency. |
| **length(ms)** | Sets both tails; the wire/noise tail is now close to the shell decay. Default: 190 ms. |
| **snappy(level)** | Snare-wire/noise amount, 0–1. Higher values give a brighter, rattling crack; lower values leave more of the shell tone. Default: 0.85. |
| **tone(level)** | Tuned shell contribution, 0–1. Default: 0.62. |

## Usage

Include Audio.h, create the objects, connect their output to a mixer, and
trigger them from loop() or a sequencer:

~~~cpp
AudioSynthKick kick;
AudioSynthTom tom;
AudioSynthSnare snare;

AudioConnection kickToMix(kick, 0, drumMixer, 0);
AudioConnection tomToMix(tom, 0, drumMixer, 1);
AudioConnection snareToMix(snare, 0, drumMixer, 2);

void setup() {
    AudioMemory(16);
    kick.frequency(52.0f);
    tom.frequency(125.0f);
    snare.frequency(185.0f);
}

void loop() {
    kick.noteOn(110);
    tom.noteOn(90);
    snare.noteOn(100);
}
~~~

Configure frequency and envelope parameters after selecting the audio sample
rate. If the sample rate is changed after construction, call the corresponding
configuration setters again so phase increments, decay coefficients and the
snare noise-filter coefficients are recalculated.

A complete sequencer example is in
examples/Synthesis/AnalogDrumKit/AnalogDrumKit.cpp. It demonstrates velocity
accents, ghost notes, hi-hat envelopes and a four-bar tom fill.
