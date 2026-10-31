// Implement an 18-voice polyphonic MIDI player. Scores for this example must
// be generated with velocity output enabled (-v).
//
// Music data is read from memory. ESP-beatsy MIDITONES V2.0 converts a MIDI
// file into this compact byte-stream format (5-bit voice IDs, velocity enabled).
//
// This example code is in the public domain.

#include <Arduino.h>
#include <Audio.h>

#include "PlaySynthMusic.h"

static const unsigned int NUM_VOICES = 18;
#define AMPLITUDE (0.2)

const unsigned char *sp = score;


// ============================================================================
// Voice generators and envelopes
// ============================================================================

AudioSynthWaveform waves[NUM_VOICES];
AudioEffectEnvelope envs[NUM_VOICES];

short wave_type[NUM_VOICES] = {
    WAVEFORM_SINE, WAVEFORM_SQUARE, WAVEFORM_SAWTOOTH, WAVEFORM_TRIANGLE,
    WAVEFORM_SINE, WAVEFORM_SQUARE, WAVEFORM_SAWTOOTH, WAVEFORM_TRIANGLE,
    WAVEFORM_SINE, WAVEFORM_SQUARE, WAVEFORM_SAWTOOTH, WAVEFORM_TRIANGLE,
    WAVEFORM_SINE, WAVEFORM_SQUARE, WAVEFORM_SAWTOOTH, WAVEFORM_TRIANGLE,
    WAVEFORM_SINE, WAVEFORM_SQUARE
};


// ============================================================================
// AudioMixerN: one 18-input mixer per output channel
//
// AudioMixerN accumulates the voices in float and saturates once at the final
// 16-bit output. The V2.0 MIDI format carries a 5-bit voice index (0..31);
// this example instantiates 18 voices. The fixed AudioConnection arrays are
// wired in setup().
// ============================================================================

AudioMixerN mixerLeft(NUM_VOICES);
AudioMixerN mixerRight(NUM_VOICES);

AudioConnection voiceToEnvelope[NUM_VOICES];
AudioConnection voiceToLeft[NUM_VOICES];
AudioConnection voiceToRight[NUM_VOICES];

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

AudioConnection mixerLeftToOutput(mixerLeft, 0, audioOut, 0);
AudioConnection mixerRightToOutput(mixerRight, 0, audioOut, 1);


// ============================================================================
// Codec
// ============================================================================

#if defined(AUDIO_CODEC_ES8388)
AudioControlES8388 codec({PIN_I2C_SDA, PIN_I2C_SCL, PIN_AMPLIFIER, PIN_AMPLIFIER_ACTIVE});
#elif defined(AUDIO_CODEC_ES8311)
AudioControlES8311 codec({PIN_I2C_SDA, PIN_I2C_SCL, PIN_AMPLIFIER, PIN_AMPLIFIER_ACTIVE});
#else
#error "AUDIO_CODEC not defined"
#endif



// ============================================================================
// Initial volume
// ============================================================================

int volume = 205;


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

    AudioMemory(26);

    // Connect every voice to its envelope and to both stereo mixers.
    for (unsigned int i = 0; i < NUM_VOICES; ++i) {
        voiceToEnvelope[i].connect(waves[i], envs[i]);
        voiceToLeft[i].connect(envs[i], 0, mixerLeft, i);
        voiceToRight[i].connect(envs[i], 0, mixerRight, i);
    }


    // ------------------------------------------------------------------------
    // Codec
    // ------------------------------------------------------------------------

    codec.enable();
    codec.volume((float)volume / 1023.0f);


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

    for (unsigned int i = 0; i < NUM_VOICES; ++i) {

        envs[i].attack(9.2);
        envs[i].hold(2.1);
        envs[i].decay(31.4);
        envs[i].sustain(0.6);
        envs[i].release(84.5);

        // Uncomment these to hear without envelope effects.

        // envs[i].attack(0.0);
        // envs[i].hold(0.0);
        // envs[i].decay(0.0);
        // envs[i].release(0.0);
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

    opcode = c & CMD_CLASS_MASK;
    chan   = c & CMD_VOICE_MASK;


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

    if (c == CMD_STOP) {

        for (chan = 0; chan < NUM_VOICES; chan++) {

            envs[chan].noteOff();
            waves[chan].amplitude(0);
        }

        Serial.println("DONE");

        while (1);
    }


    // ------------------------------------------------------------------------
    // Stop note
    // ------------------------------------------------------------------------

    if (opcode == CMD_STOPNOTE) {

        envs[chan].noteOff();

        return;
    }


    // ------------------------------------------------------------------------
    // Play note
    // ------------------------------------------------------------------------

    if (opcode == CMD_PLAYNOTE) {

        unsigned char note = *sp++;
        unsigned char velocity = *sp++;

        AudioNoInterrupts();

        waves[chan].begin(
            AMPLITUDE * velocity2amplitude[velocity - 1],
            tune_frequencies2_PGM[note],
            wave_type[chan]
        );

        envs[chan].noteOn();

        AudioInterrupts();

        return;
    }


    // ------------------------------------------------------------------------
    // Restart tune
    // ------------------------------------------------------------------------

    if (c == CMD_RESTART) {

        sp = score;

        return;
    }
}
