
#pragma once

#include <Arduino.h>
#include <HTTPClient.h>
#include <strings.h>
#include <esp_log.h>

namespace Radio {

enum class AudioCodec : uint8_t { Unknown, MP3, AAC, Opus };

constexpr const char *codecName(AudioCodec codec) {
    switch (codec) {
    case AudioCodec::MP3:
        return "MP3";
    case AudioCodec::AAC:
        return "AAC";
    case AudioCodec::Opus:
        return "Opus";
    default:
        return "Unknown";
    }
}

inline bool contentTypeMatches(const char *contentType, const char *mimeType) {
    const size_t len = strlen(mimeType);

    if (strncasecmp(contentType, mimeType, len) != 0) return false;

    const char *p = contentType + len;

    // Optional whitespace nach dem MIME-Type
    while (*p == ' ' || *p == '\t') ++p;

    // Exakt beendet oder MIME-Parameter folgen
    return *p == '\0' || *p == ';';
}

inline AudioCodec detectCodec(HTTPClient &http) {
    struct ContentType {
        const char *mime;
        AudioCodec codec;
    };

    static constexpr ContentType contentTypes[] = //
        {{"audio/aac", AudioCodec::AAC},
         {"audio/aacp", AudioCodec::AAC},
         {"audio/ogg", AudioCodec::Opus},
         {"application/ogg", AudioCodec::Opus},
         {"audio/mpeg", AudioCodec::MP3}};

    String contentType = http.header("content-type");
    contentType.trim();

    const char *p = contentType.c_str();

    for (const auto &type : contentTypes) {
        if (contentTypeMatches(p, type.mime)) {
            ESP_LOGI("RADIO", "Detected stream codec: %s", codecName(type.codec));
            return type.codec;
        }
    }

    ESP_LOGE("RADIO", "Unknown stream codec: %s", p);
    return AudioCodec::Unknown;
}

} // namespace Radio
