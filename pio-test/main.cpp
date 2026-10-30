#include <Arduino.h>
#include <AudioStream.h>

// Minimal PlatformIO compile test for the library's core AudioStream API.
// Keep board-specific audio tests and hardware initialization in this folder.

static_assert(AUDIO_BLOCK_SAMPLES == 128, "Unexpected audio block size");

void setup() {
}

void loop() {
}
