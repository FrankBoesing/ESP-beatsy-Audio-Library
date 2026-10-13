#pragma once

#include "AudioStream.h"

namespace audio_detail {

// Serializes graph/lifecycle operations with AudioTask::update().
// The underlying AudioStream mutex is recursive, so nested calls are safe.
class GraphUpdateLock {
  public:
    GraphUpdateLock() { AudioNoInterrupts(); }
    ~GraphUpdateLock() { AudioInterrupts(); }

    GraphUpdateLock(const GraphUpdateLock &) = delete;
    GraphUpdateLock &operator=(const GraphUpdateLock &) = delete;
};

} // namespace audio_detail
