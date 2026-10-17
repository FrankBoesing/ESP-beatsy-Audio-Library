#pragma once

#include "AudioStream.h"
#include "softcodecs/AudioSource.h"

enum class AudioCodec : uint8_t { Unknown, MP3, AAC, WAV, Opus };

struct AudioPlayerInfo {
    AudioCodec codec;
    const char *name;

    const char *const *mimeTypes;
    uint8_t mimeTypeCount;
};

class AudioPlayer : public AudioStream {
  public:
    virtual ~AudioPlayer() = default;

    virtual const AudioPlayerInfo &info() const = 0;

    AudioCodec codec() const { return info().codec; }
    const char *name() const { return info().name; }

    virtual bool play(AudioSource &source) = 0;
    virtual void stop() = 0;
    virtual bool isPlaying() const = 0;

    virtual uint32_t positionMillis() const = 0;
    virtual uint32_t lengthMillis() const = 0;

    // Error reporting. Not every player has a persistent error code.
    virtual bool hasLastError() const { return false; }
    virtual int lastError() const { return 0; }

    // Optional decoder performance metrics.
    virtual bool hasDecoderMetrics() const { return false; }

#if SOFTCODEC_METRICS
    virtual float decodeProcessorUsage() const { return 0.0f; }
    virtual float decodeProcessorUsageMax() const { return 0.0f; }
#endif

    bool matchesContentType(const char *contentType) const {
        if (contentType == nullptr) return false;

        const AudioPlayerInfo &playerInfo = info();

        for (uint8_t i = 0; i < playerInfo.mimeTypeCount; ++i) {
            if (mimeMatches(contentType, playerInfo.mimeTypes[i])) return true;
        }

        return false;
    }

  protected:

    AudioPlayer() : AudioStream(0, nullptr) {}

    static bool mimeMatches(const char *contentType, const char *mimeType) {
        const size_t len = strlen(mimeType);
        if (strncasecmp(contentType, mimeType, len) != 0) return false;
        const char *p = contentType + len;
        while (*p == ' ' || *p == '\t') ++p;
        return *p == '\0' || *p == ';';
    }
};
