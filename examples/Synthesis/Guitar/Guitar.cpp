#include <Arduino.h>
#include <Audio.h>

#include "chords.h"

// Special thanks to Matthew Rahtz - http://amid.fish/karplus-strong/

#ifdef AUDIO_OUTPUT_USB
AudioOutputUSB audio_out;

#else

AudioOutputI2S audio_out({
    PIN_I2S_BLCK, // BCLK
    PIN_I2S_WS,   // WS / LRCLK
    PIN_I2S_DOUT, // DOUT
    PIN_I2S_MLCK  // MCLK
});

#if defined(AUDIO_CODEC_ES8388)
AudioControlES8388 codec({PIN_I2C_SDA, PIN_I2C_SCL, PIN_AMPLIFIER, PIN_AMPLIFIER_ACTIVE});
#elif defined(AUDIO_CODEC_ES8311)
AudioControlES8311 codec({PIN_I2C_SDA, PIN_I2C_SCL, PIN_AMPLIFIER, PIN_AMPLIFIER_ACTIVE});
#else
#ifndef AUDIO_CODEC_NONE
#warning "AUDIO_CODEC not defined"
#endif
#endif

#endif // AUDIO_OUTPUT_USB

AudioSynthKarplusStrong string1;
AudioSynthKarplusStrong string2;
AudioSynthKarplusStrong string3;
AudioSynthKarplusStrong string4;
AudioSynthKarplusStrong string5;
AudioSynthKarplusStrong string6;

AudioMixer4 mixer1;
AudioMixer4 mixer2;

AudioConnection patchCord1(string1, 0, mixer1, 0);
AudioConnection patchCord2(string2, 0, mixer1, 1);
AudioConnection patchCord3(string3, 0, mixer1, 2);
AudioConnection patchCord4(string4, 0, mixer1, 3);
AudioConnection patchCord5(mixer1, 0, mixer2, 0);
AudioConnection patchCord6(string5, 0, mixer2, 1);
AudioConnection patchCord7(string6, 0, mixer2, 2);
AudioConnection patchCord8(mixer2, 0, audio_out, 0);
AudioConnection patchCord9(mixer2, 0, audio_out, 1);

const int finger_delay = 5;
const int hand_delay = 220;

int chordnum = 0;


// Damping follows the physical guitar string (low E to high E), not the
// AudioSynthKarplusStrong instance. strum_dn() reverses the voice assignment.
static const float stringDamping[6] = {
    0.45f, // low E2
    0.38f, // A2
    0.30f, // D3
    0.22f, // G3
    0.14f, // B3
    0.10f  // high E4
};

static void pluckString(AudioSynthKarplusStrong &voice, int stringIndex,
                        float frequency, float velocity) {
    voice.damping(stringDamping[stringIndex]);
    voice.noteOn(frequency, velocity);
}

void strum_up(const float *chord, float velocity) {
    if (chord[0] > 20.0f) pluckString(string1, 0, chord[0], velocity);
    delay(finger_delay);
    if (chord[1] > 20.0f) pluckString(string2, 1, chord[1], velocity);
    delay(finger_delay);
    if (chord[2] > 20.0f) pluckString(string3, 2, chord[2], velocity);
    delay(finger_delay);
    if (chord[3] > 20.0f) pluckString(string4, 3, chord[3], velocity);
    delay(finger_delay);
    if (chord[4] > 20.0f) pluckString(string5, 4, chord[4], velocity);
    delay(finger_delay);
    if (chord[5] > 20.0f) pluckString(string6, 5, chord[5], velocity);
    delay(finger_delay);
}

void strum_dn(const float *chord, float velocity) {
    if (chord[5] > 20.0f) pluckString(string1, 5, chord[5], velocity);
    delay(finger_delay);
    if (chord[4] > 20.0f) pluckString(string2, 4, chord[4], velocity);
    delay(finger_delay);
    if (chord[3] > 20.0f) pluckString(string3, 3, chord[3], velocity);
    delay(finger_delay);
    if (chord[2] > 20.0f) pluckString(string4, 2, chord[2], velocity);
    delay(finger_delay);
    if (chord[1] > 20.0f) pluckString(string5, 1, chord[1], velocity);
    delay(finger_delay);
    if (chord[0] > 20.0f) pluckString(string6, 0, chord[0], velocity);
    delay(finger_delay);
}

void setup() {
    // AudioOutputUSB initializes USB Audio and CDC during AudioMemory().
    AudioMemory(15);

    Serial.begin(115200);
    delay(2000);

#ifndef AUDIO_CODEC_NONE
    codec.enable();
    codec.volume(0.2f);
#endif

    const float gain = 0.15f;
    mixer1.gain(0, gain);
    mixer1.gain(1, gain);
    mixer1.gain(2, gain);
    mixer1.gain(3, gain);
    mixer2.gain(0, gain); // mixer1 output: strings 1–4
    mixer2.gain(1, gain);
    mixer2.gain(2, gain);

}

void loop() {
    const float *chord;

    // Each time through the loop, play a different chord.
    if (chordnum == 0) {
        chord = Cmajor;
        Serial.println("C major");
        chordnum = 1;
    } else if (chordnum == 1) {
        chord = Gmajor;
        Serial.println("G major");
        chordnum = 2;
    } else if (chordnum == 2) {
        chord = Aminor;
        Serial.println("A minor");
        chordnum = 3;
    } else {
        chord = Eminor;
        Serial.println("E minor");
        chordnum = 0;
    }

    // Strum the six strings several times.
    strum_up(chord, 1.0f);
    delay(hand_delay);
    delay(hand_delay);
    strum_up(chord, 1.0f);
    delay(hand_delay);
    strum_dn(chord, 0.8f);
    delay(hand_delay);
    delay(hand_delay);
    strum_dn(chord, 0.8f);
    delay(hand_delay);
    strum_up(chord, 1.0f);
    delay(hand_delay);
    strum_dn(chord, 0.8f);
    delay(hand_delay);
    strum_up(chord, 1.0f);
    delay(hand_delay);
    delay(hand_delay);
    strum_up(chord, 1.0f);
    delay(hand_delay);
    strum_dn(chord, 0.7f);
    delay(hand_delay);
    delay(hand_delay);
    strum_dn(chord, 0.7f);
    delay(hand_delay);
    strum_up(chord, 1.0f);
    delay(hand_delay);
    strum_dn(chord, 0.7f);
    delay(hand_delay);

    Serial.print("Max CPU Usage = ");
    Serial.print(AudioProcessorUsageMax(), 1);
    Serial.println("%");
}


