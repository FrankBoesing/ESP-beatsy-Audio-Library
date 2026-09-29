#include "AudioInputBuffer.h"

#if defined(ARDUINO_ARCH_ESP32)

#include "esp_heap_caps.h"

#endif


AudioInputBuffer::AudioInputBuffer(
    size_t capacity
)
    : buffer(nullptr),
      buffer_size(capacity),

      region_a_start(0),
      region_a_length(0),

      region_b_start(0),
      region_b_length(0),

      write_acquired(false),
      write_start(0),
      write_length(0),

      psram_allocated(false)
{
}


AudioInputBuffer::~AudioInputBuffer()
{
    end();
}


// ============================================================================
// Lifetime
// ============================================================================

bool AudioInputBuffer::begin()
{
    end();

    if (buffer_size == 0) {
        return false;
    }

    if (!allocateBuffer()) {
        return false;
    }

    reset();

    return true;
}


void AudioInputBuffer::end()
{
    freeBuffer();

    reset();
}


void AudioInputBuffer::reset()
{
    region_a_start = 0;
    region_a_length = 0;

    region_b_start = 0;
    region_b_length = 0;

    write_acquired = false;
    write_start = 0;
    write_length = 0;
}


// ============================================================================
// Memory allocation
// ============================================================================

bool AudioInputBuffer::allocateBuffer()
{
#if defined(ARDUINO_ARCH_ESP32)

    /*
     * PSRAM bevorzugen.
     */
    if (psramFound()) {

        buffer =
            static_cast<uint8_t *>(
                heap_caps_malloc(
                    buffer_size,
                    MALLOC_CAP_SPIRAM |
                    MALLOC_CAP_8BIT
                )
            );

        if (buffer != nullptr) {

            psram_allocated = true;

            return true;
        }
    }

#endif


    /*
     * Fallback auf normalen Heap.
     */
    buffer =
        static_cast<uint8_t *>(
            malloc(buffer_size)
        );

    if (buffer == nullptr) {

        psram_allocated = false;

        return false;
    }

    psram_allocated = false;

    return true;
}


void AudioInputBuffer::freeBuffer()
{
    if (buffer == nullptr) {
        return;
    }

#if defined(ARDUINO_ARCH_ESP32)

    heap_caps_free(buffer);

#else

    free(buffer);

#endif

    buffer = nullptr;
    psram_allocated = false;
}


// ============================================================================
// Read side
// ============================================================================

const uint8_t *AudioInputBuffer::acquireRead(
    size_t &length
) const
{
    length = 0;

    if (buffer == nullptr) {
        return nullptr;
    }

    if (region_a_length == 0) {
        return nullptr;
    }

    length = region_a_length;

    return buffer + region_a_start;
}


bool AudioInputBuffer::releaseRead(
    size_t consumed
)
{
    if (consumed > region_a_length) {
        return false;
    }

    if (consumed == 0) {
        return true;
    }


    /*
     * Region A wird einfach logisch verkürzt.
     *
     * KEIN memcpy().
     */
    region_a_start += consumed;
    region_a_length -= consumed;


    /*
     * Region A vollständig verbraucht.
     */
    if (region_a_length == 0) {

        if (region_b_length > 0) {

            /*
             * Region B wird zu Region A.
             *
             * Auch hier:
             *
             * KEIN memcpy().
             */
            region_a_start =
                region_b_start;

            region_a_length =
                region_b_length;

            region_b_start = 0;
            region_b_length = 0;

        }
        else {

            /*
             * Buffer vollständig leer.
             */
            region_a_start = 0;
            region_a_length = 0;

            region_b_start = 0;
            region_b_length = 0;
        }
    }

    return validate();
}


// ============================================================================
// Write side
// ============================================================================

uint8_t *AudioInputBuffer::getWriteRegion(
    size_t &length
)
{
    length = 0;

    if (buffer == nullptr) {
        return nullptr;
    }


    /*
     * ------------------------------------------------------------------------
     * Region B existiert bereits.
     *
     * Dann muss weiter in Region B geschrieben werden.
     *
     * Layout:
     *
     *   [ B ][ free ][ A ]
     *
     * wobei B am Anfang des Buffers liegt.
     * ------------------------------------------------------------------------
     */
    if (region_b_length > 0) {

        /*
         * Region B beginnt normalerweise bei 0.
         */
        const size_t end =
            region_b_start +
            region_b_length;

        /*
         * Freier Bereich vor Region A.
         */
        if (region_a_start > end) {

            length =
                region_a_start - end;

            return buffer + end;
        }

        return nullptr;
    }


    /*
     * ------------------------------------------------------------------------
     * Keine Region B.
     *
     * Wir können entweder hinter A schreiben oder
     * - falls dort mehr Platz vorhanden ist -
     * eine neue Region B am Anfang beginnen.
     * ------------------------------------------------------------------------
     */

    const size_t end_of_a =
        region_a_start +
        region_a_length;


    /*
     * Freier Bereich hinter A.
     */
    const size_t free_after_a =
        buffer_size -
        end_of_a;


    /*
     * Freier Bereich vor A.
     *
     * Nur relevant, wenn A nicht bei 0 beginnt.
     */
    const size_t free_before_a =
        region_a_start;


    /*
     * Wenn der Buffer leer ist, benutzen wir den gesamten
     * Buffer als Region A.
     */
    if (region_a_length == 0) {

        length = buffer_size;

        return buffer;
    }


    /*
     * Hinter Region A schreiben, wenn dort Platz ist.
     *
     * Das hält den normalen SD-/Filesystem-Pfad linear.
     */
    if (free_after_a > 0) {

        /*
         * Wenn vorne deutlich mehr Platz vorhanden ist,
         * können wir stattdessen Region B beginnen.
         *
         * Für den ersten Entwurf bevorzugen wir jedoch
         * den Bereich hinter A.
         */
        if (free_after_a >= free_before_a) {

            length = free_after_a;

            return buffer + end_of_a;
        }
    }


    /*
     * Vor Region A kann eine neue Region B entstehen.
     *
     * Layout:
     *
     *   [ free / B ][ A ]
     *
     * Die Region B wird später nach vollständigem Verbrauch
     * von A automatisch zu A.
     */
    if (free_before_a > 0) {

        region_b_start = 0;

        length = free_before_a;

        return buffer;
    }


    return nullptr;
}


uint8_t *AudioInputBuffer::acquireWrite(
    size_t &length
)
{
    if (write_acquired) {

        /*
         * acquireWrite() darf nicht zweimal ohne
         * commitWrite() aufgerufen werden.
         */
        length = 0;

        return nullptr;
    }


    uint8_t *ptr =
        getWriteRegion(length);

    if (ptr == nullptr ||
        length == 0)
    {
        return nullptr;
    }


    write_acquired = true;

    write_start =
        static_cast<size_t>(
            ptr - buffer
        );

    write_length = length;

    return ptr;
}


bool AudioInputBuffer::commitWrite(
    size_t written
)
{
    if (!write_acquired) {
        return false;
    }

    if (written > write_length) {

        write_acquired = false;
        write_start = 0;
        write_length = 0;

        return false;
    }


    /*
     * Nichts geschrieben.
     */
    if (written == 0) {

        write_acquired = false;
        write_start = 0;
        write_length = 0;

        return true;
    }


    /*
     * Prüfen, welche Region geschrieben wurde.
     *
     * ------------------------------------------------------------------------
     * Buffer war leer.
     * ------------------------------------------------------------------------
     */
    if (region_a_length == 0 &&
        region_b_length == 0)
    {
        region_a_start =
            write_start;

        region_a_length =
            written;
    }

    /*
     * ------------------------------------------------------------------------
     * Region B existiert.
     * ------------------------------------------------------------------------
     */
    else if (region_b_length > 0) {

        /*
         * Der Schreibbereich muss direkt hinter B liegen.
         */
        const size_t expected =
            region_b_start +
            region_b_length;

        if (write_start != expected) {

            write_acquired = false;
            write_start = 0;
            write_length = 0;

            return false;
        }

        region_b_length += written;
    }

    /*
     * ------------------------------------------------------------------------
     * Keine Region B.
     *
     * Prüfen, ob wir an A angehängt haben oder
     * eine neue B-Region begonnen haben.
     * ------------------------------------------------------------------------
     */
    else {

        const size_t end_of_a =
            region_a_start +
            region_a_length;


        /*
         * Append hinter Region A.
         */
        if (write_start == end_of_a) {

            region_a_length += written;
        }

        /*
         * Neue Region B am Anfang.
         */
        else if (write_start == 0 &&
                 region_a_start > 0)
        {
            region_b_start = 0;
            region_b_length = written;
        }

        else {

            write_acquired = false;
            write_start = 0;
            write_length = 0;

            return false;
        }
    }


    write_acquired = false;
    write_start = 0;
    write_length = 0;

    return validate();
}


// ============================================================================
// Source fill
// ============================================================================

AudioSourceStatus AudioInputBuffer::fill(
    AudioSource &source
)
{
    size_t available = 0;

    uint8_t *destination =
        acquireWrite(available);

    if (destination == nullptr ||
        available == 0)
    {
        /*
         * Kein freier zusammenhängender Bereich.
         */
        return AudioSourceStatus::DATA;
    }


    size_t received = 0;

    const AudioSourceStatus status =
        source.read(
            destination,
            available,
            received
        );


    /*
     * Commit auch bei received == 0,
     * damit der acquire-Zustand sauber beendet wird.
     */
    if (!commitWrite(received)) {

        return AudioSourceStatus::ERROR;
    }


    return status;
}


// ============================================================================
// Information
// ============================================================================

size_t AudioInputBuffer::availableRead() const
{
    /*
     * Nur die aktuell zusammenhängende Leseregion.
     *
     * Region B wird erst dann zu Region A,
     * wenn Region A vollständig konsumiert wurde.
     */
    return region_a_length;
}


size_t AudioInputBuffer::availableWrite() const
{
    if (buffer == nullptr) {
        return 0;
    }

    return
        buffer_size -
        region_a_length -
        region_b_length;
}

size_t AudioInputBuffer::capacity() const
{
    return buffer_size;
}


bool AudioInputBuffer::empty() const
{
    return
        region_a_length == 0 &&
        region_b_length == 0;
}


bool AudioInputBuffer::full() const
{
    return availableWrite() == 0;
}


bool AudioInputBuffer::usingPSRAM() const
{
    return psram_allocated;
}


// ============================================================================
// Validation
// ============================================================================

bool AudioInputBuffer::validate() const
{
    if (buffer == nullptr) {
        return true;
    }


    /*
     * Keine Region darf über das Bufferende hinausgehen.
     */
    if (region_a_start > buffer_size) {
        return false;
    }

    if (region_b_start > buffer_size) {
        return false;
    }


    if (region_a_length >
        buffer_size - region_a_start)
    {
        return false;
    }

    if (region_b_length >
        buffer_size - region_b_start)
    {
        return false;
    }


    /*
     * A und B dürfen sich nicht überschneiden.
     */
    if (region_a_length > 0 &&
        region_b_length > 0)
    {
        const size_t a_end =
            region_a_start +
            region_a_length;

        const size_t b_end =
            region_b_start +
            region_b_length;


        const bool overlap =
            (region_a_start < b_end) &&
            (region_b_start < a_end);

        if (overlap) {
            return false;
        }
    }


    /*
     * Der Schreibbereich darf nicht gleichzeitig
     * außerhalb des gültigen Buffers liegen.
     */
    if (write_acquired) {

        if (write_start > buffer_size) {
            return false;
        }

        if (write_length >
            buffer_size - write_start)
        {
            return false;
        }
    }


    return true;
}
