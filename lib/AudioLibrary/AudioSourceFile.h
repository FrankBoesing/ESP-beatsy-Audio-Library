#pragma once

#include "AudioSource.h"
#include <FS.h>

class AudioSourceFile : public AudioSource {
public:
    AudioSourceFile();
    explicit AudioSourceFile(const File &file);

    ~AudioSourceFile() override;

    /*
     * Eine bereits geöffnete Arduino-File übernehmen.
     */
    bool open(const File &file);

    /*
     * Datei über ein FS-Objekt öffnen.
     *
     * Funktioniert z.B. mit:
     *
     *   SD
     *   SD_MMC
     *   LittleFS
     */
    bool open(fs::FS &fs, const char *path, const char *mode = FILE_READ);

    AudioSourceStatus read(
        uint8_t *buffer,
        size_t requested,
        size_t &received
    ) override;

    uint64_t position() const override;
    uint64_t size() const override;

    bool isSeekable() const override;

    bool seek(uint64_t position) override;

    void close() override;

    bool isOpen() const override;

    File &file();
    const File &file() const;

private:
    File _file;
};
