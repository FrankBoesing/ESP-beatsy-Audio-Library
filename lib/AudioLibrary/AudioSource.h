#pragma once

#include <Arduino.h>

enum class AudioSourceStatus : uint8_t {
    DATA,
    WOULD_BLOCK,
    END_OF_STREAM,
    ERROR
};

class AudioSource {
public:
    virtual ~AudioSource() = default;

    /*
     * Liefert bis zu 'requested' Bytes.
     *
     * received:
     *   Anzahl tatsächlich gelieferter Bytes.
     *
     * Rückgabewert:
     *   DATA          - mindestens ein Byte wurde geliefert
     *   WOULD_BLOCK   - momentan keine Daten verfügbar,
     *                   Quelle ist aber noch nicht beendet
     *   END_OF_STREAM - Quelle ist endgültig am Ende
     *   ERROR         - Fehler
     */
    virtual AudioSourceStatus read(
        uint8_t *buffer,
        size_t requested,
        size_t &received
    ) = 0;

    /*
     * Position innerhalb der Quelle.
     *
     * Bei nicht seekbaren Quellen kann 0 zurückgegeben werden.
     */
    virtual uint64_t position() const = 0;

    /*
     * Größe der Quelle.
     *
     * Bei unbekannter Größe wird 0 zurückgegeben.
     */
    virtual uint64_t size() const = 0;

    /*
     * Ist die Quelle seekbar?
     */
    virtual bool isSeekable() const = 0;

    /*
     * Position verändern.
     *
     * Rückgabe:
     *   true  = erfolgreich
     *   false = nicht unterstützt / Fehler
     */
    virtual bool seek(uint64_t position) = 0;

    /*
     * Quelle schließen.
     *
     * Standardmäßig nichts zu tun.
     */
    virtual void close() {}

    /*
     * Quelle öffnen bzw. initialisieren.
     *
     * Nicht jede Source benötigt einen separaten open()-Aufruf.
     */
    virtual bool isOpen() const = 0;
};
