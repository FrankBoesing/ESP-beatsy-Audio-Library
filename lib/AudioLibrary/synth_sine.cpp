#include "synth_sine.h"
#include "sine_table.h"

OSPEED
void AudioSynthWaveformSine::update() {
    const uint32_t increment = phase_increment;

    // Keep phase progression independent of block allocation.
    if (magnitude == 0) {
        phase_accumulator +=
            increment * static_cast<uint32_t>(AUDIO_BLOCK_SAMPLES);
        return;
    }

    audio_block_t* block = allocate();

    if (!block) {
        phase_accumulator +=
            increment * static_cast<uint32_t>(AUDIO_BLOCK_SAMPLES);
        return;
    }

    uint32_t phase = phase_accumulator;

    AudioSineTable::generate(
        block->data,
        phase,
        increment,
        AUDIO_BLOCK_SAMPLES,
        magnitude);

    phase_accumulator = phase;

    transmit(block);
    release(block);
}
