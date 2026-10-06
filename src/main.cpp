// Implement a 16 note polyphonic midi player  :-)
//
// Music data is read from memory.  The "Miditones" program is used to
// convert from a MIDI file to this compact format.
//
// This example code is in the public domain.

#include <Audio.h>
#include <Wire.h>

#include "PlaySynthMusic.h"

unsigned char *sp = score;

#define AMPLITUDE (0.2)


// ============================================================================
// 16 waveforms, one for each MIDI channel
// ============================================================================

AudioSynthWaveform sine0, sine1, sine2, sine3;
AudioSynthWaveform sine4, sine5, sine6, sine7;
AudioSynthWaveform sine8, sine9, sine10, sine11;
AudioSynthWaveform sine12, sine13, sine14, sine15;

AudioSynthWaveform *waves[16] = {
    &sine0,  &sine1,  &sine2,  &sine3,
    &sine4,  &sine5,  &sine6,  &sine7,
    &sine8,  &sine9,  &sine10, &sine11,
    &sine12, &sine13, &sine14, &sine15
};


// ============================================================================
// Waveform type for each MIDI channel
// ============================================================================

short wave_type[16] = {
    WAVEFORM_SINE,
    WAVEFORM_SQUARE,
    WAVEFORM_SAWTOOTH,
    WAVEFORM_TRIANGLE,

    WAVEFORM_SINE,
    WAVEFORM_SQUARE,
    WAVEFORM_SAWTOOTH,
    WAVEFORM_TRIANGLE,

    WAVEFORM_SINE,
    WAVEFORM_SQUARE,
    WAVEFORM_SAWTOOTH,
    WAVEFORM_TRIANGLE,

    WAVEFORM_SINE,
    WAVEFORM_SQUARE,
    WAVEFORM_SAWTOOTH,
    WAVEFORM_TRIANGLE
};


// ============================================================================
// Envelope for each waveform
// ============================================================================

AudioEffectEnvelope env0, env1, env2, env3;
AudioEffectEnvelope env4, env5, env6, env7;
AudioEffectEnvelope env8, env9, env10, env11;
AudioEffectEnvelope env12, env13, env14, env15;

AudioEffectEnvelope *envs[16] = {
    &env0,  &env1,  &env2,  &env3,
    &env4,  &env5,  &env6,  &env7,
    &env8,  &env9,  &env10, &env11,
    &env12, &env13, &env14, &env15
};


// ============================================================================
// Waveform -> Envelope
// ============================================================================

AudioConnection patchCord01(sine0,  env0);
AudioConnection patchCord02(sine1,  env1);
AudioConnection patchCord03(sine2,  env2);
AudioConnection patchCord04(sine3,  env3);

AudioConnection patchCord05(sine4,  env4);
AudioConnection patchCord06(sine5,  env5);
AudioConnection patchCord07(sine6,  env6);
AudioConnection patchCord08(sine7,  env7);

AudioConnection patchCord09(sine8,  env8);
AudioConnection patchCord10(sine9,  env9);
AudioConnection patchCord11(sine10, env10);
AudioConnection patchCord12(sine11, env11);

AudioConnection patchCord13(sine12, env12);
AudioConnection patchCord14(sine13, env13);
AudioConnection patchCord15(sine14, env14);
AudioConnection patchCord16(sine15, env15);


// ============================================================================
// 16-channel floating-point mixers
//
// AudioMixerN performs:
//   - float gain
//   - float accumulation
//   - one final saturation to int16 PCM
//
// One mixer is used for each output channel.
// ============================================================================

AudioMixerN mixerLeft(16);
AudioMixerN mixerRight(16);


// ============================================================================
// Mix all 16 voices to LEFT
// ============================================================================

AudioConnection patchCord17(env0,  0, mixerLeft,  0);
AudioConnection patchCord18(env1,  0, mixerLeft,  1);
AudioConnection patchCord19(env2,  0, mixerLeft,  2);
AudioConnection patchCord20(env3,  0, mixerLeft,  3);

AudioConnection patchCord21(env4,  0, mixerLeft,  4);
AudioConnection patchCord22(env5,  0, mixerLeft,  5);
AudioConnection patchCord23(env6,  0, mixerLeft,  6);
AudioConnection patchCord24(env7,  0, mixerLeft,  7);

AudioConnection patchCord25(env8,  0, mixerLeft,  8);
AudioConnection patchCord26(env9,  0, mixerLeft,  9);
AudioConnection patchCord27(env10, 0, mixerLeft, 10);
AudioConnection patchCord28(env11, 0, mixerLeft, 11);

AudioConnection patchCord29(env12, 0, mixerLeft, 12);
AudioConnection patchCord30(env13, 0, mixerLeft, 13);
AudioConnection patchCord31(env14, 0, mixerLeft, 14);
AudioConnection patchCord32(env15, 0, mixerLeft, 15);


// ============================================================================
// Mix all 16 voices to RIGHT
// ============================================================================

AudioConnection patchCord33(env0,  0, mixerRight,  0);
AudioConnection patchCord34(env1,  0, mixerRight,  1);
AudioConnection patchCord35(env2,  0, mixerRight,  2);
AudioConnection patchCord36(env3,  0, mixerRight,  3);

AudioConnection patchCord37(env4,  0, mixerRight,  4);
AudioConnection patchCord38(env5,  0, mixerRight,  5);
AudioConnection patchCord39(env6,  0, mixerRight,  6);
AudioConnection patchCord40(env7,  0, mixerRight,  7);

AudioConnection patchCord41(env8,  0, mixerRight,  8);
AudioConnection patchCord42(env9,  0, mixerRight,  9);
AudioConnection patchCord43(env10, 0, mixerRight, 10);
AudioConnection patchCord44(env11, 0, mixerRight, 11);

AudioConnection patchCord45(env12, 0, mixerRight, 12);
AudioConnection patchCord46(env13, 0, mixerRight, 13);
AudioConnection patchCord47(env14, 0, mixerRight, 14);
AudioConnection patchCord48(env15, 0, mixerRight, 15);


// ============================================================================
// Audio output
// ============================================================================

AudioOutputI2S audioOut({
    27, // BCLK
    25, // WS / LRCLK
    26, // DOUT
    0   // MCLK
});


// ============================================================================
// Mixer -> Audio output
// ============================================================================

AudioConnection patchCord49(mixerLeft,  0, audioOut, 0);
AudioConnection patchCord50(mixerRight, 0, audioOut, 1);


// ============================================================================
// Codec
// ============================================================================

AudioControlES8388 codec;


// ============================================================================
// Initial volume
// ============================================================================

int volume = 80;


// ============================================================================
// Setup
// ============================================================================

void setup()
{
    Serial.begin(115200);

    delay(200);


    Serial.print("Begin ");
    Serial.println(__FILE__);


    // ------------------------------------------------------------------------
    // Audio memory
    // ------------------------------------------------------------------------

    AudioMemory(22);


    // ------------------------------------------------------------------------
    // Codec
    // ------------------------------------------------------------------------

    codec.enable();
    codec.volume(volume);


    // ------------------------------------------------------------------------
    // Stereo positioning
    //
    // Same channel distribution as the original example:
    //
    // Left:
    //   channel 1 and 3 reduced to 0.36
    //
    // Right:
    //   channel 0 and 2 reduced to 0.36
    //
    // All other channels remain at unity gain.
    // ------------------------------------------------------------------------

    mixerLeft.gain(1, 0.36f);
    mixerLeft.gain(3, 0.36f);

    mixerRight.gain(0, 0.36f);
    mixerRight.gain(2, 0.36f);


    // ------------------------------------------------------------------------
    // Envelope parameters
    // ------------------------------------------------------------------------

    for (int i = 0; i < 16; i++) {

        envs[i]->attack(9.2);
        envs[i]->hold(2.1);
        envs[i]->decay(31.4);
        envs[i]->sustain(0.6);
        envs[i]->release(84.5);

        // Uncomment these to hear without envelope effects.

        // envs[i]->attack(0.0);
        // envs[i]->hold(0.0);
        // envs[i]->decay(0.0);
        // envs[i]->release(0.0);
    }


    Serial.println("setup done");


    // ------------------------------------------------------------------------
    // Initialize processor and memory measurements
    // ------------------------------------------------------------------------

    AudioProcessorUsageMaxReset();
    AudioMemoryUsageMaxReset();
}


// ============================================================================
// Main loop timing
// ============================================================================

unsigned long last_time = millis();


void loop()
{
    unsigned char c;
    unsigned char opcode;
    unsigned char chan;

    unsigned long d_time;


    // ------------------------------------------------------------------------
    // Processor / memory statistics
    // ------------------------------------------------------------------------

    if (1) {

        if (millis() - last_time >= 5000) {

            Serial.print("Proc = ");
            Serial.print(AudioProcessorUsage());

            Serial.print(" (");
            Serial.print(AudioProcessorUsageMax());

            Serial.print("),  Mem = ");
            Serial.print(AudioMemoryUsage());

            Serial.print(" (");
            Serial.print(AudioMemoryUsageMax());

            Serial.println(")");

            last_time = millis();
        }
    }


    // ------------------------------------------------------------------------
    // Volume control
    //
    // Uncomment if you have a volume pot soldered to your audio shield.
    // ------------------------------------------------------------------------

    /*
    int n = analogRead(15);

    if (n != volume) {

        volume = n;
        codec.volume((float)n / 1023);
    }
    */


    // ------------------------------------------------------------------------
    // Read next note from table
    // ------------------------------------------------------------------------

    c = *sp++;

    opcode = c & 0xF0;
    chan   = c & 0x0F;


    // ------------------------------------------------------------------------
    // Delay
    // ------------------------------------------------------------------------

    if (c < 0x80) {

        d_time = (c << 8) | *sp++;

        delay(d_time);

        return;
    }


    // ------------------------------------------------------------------------
    // End of song
    // ------------------------------------------------------------------------

    if (*sp == CMD_STOP) {

        for (chan = 0; chan < 10; chan++) {

            envs[chan]->noteOff();
            waves[chan]->amplitude(0);
        }

        Serial.println("DONE");

        while (1);
    }


    // ------------------------------------------------------------------------
    // Stop note
    // ------------------------------------------------------------------------

    if (opcode == CMD_STOPNOTE) {

        envs[chan]->noteOff();

        return;
    }


    // ------------------------------------------------------------------------
    // Play note
    // ------------------------------------------------------------------------

    if (opcode == CMD_PLAYNOTE) {

        unsigned char note = *sp++;
        unsigned char velocity = *sp++;

        AudioNoInterrupts();

        waves[chan]->begin(
            AMPLITUDE * velocity2amplitude[velocity - 1],
            tune_frequencies2_PGM[note],
            wave_type[chan]
        );

        envs[chan]->noteOn();

        AudioInterrupts();

        return;
    }


    // ------------------------------------------------------------------------
    // Restart tune
    // ------------------------------------------------------------------------

    if (opcode == CMD_RESTART) {

        sp = score;

        return;
    }
}
