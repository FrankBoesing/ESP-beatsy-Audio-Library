
#include "play_mp3.h"

#include <esp_log.h>

static const char *TAG = "AudioPlayMp3";

// Debug counters. Keep the logging bounded so that Serial output does not
// become the timing bottleneck of the decoder.
static uint32_t g_decodeCalls = 0;
static uint32_t g_decodedFrames = 0;
static uint32_t g_syncSearches = 0;
static uint32_t g_txBlocks = 0;


// ============================================================================
// Constructor
// ============================================================================

AudioPlayMp3::AudioPlayMp3()
    : AudioStream(0, nullptr),

      state(STOPPED),

      source(nullptr),
      own_source(false),

      inputBuffer(AUDIO_MP3_INPUT_BUFFER_SIZE),

      decoder(nullptr),

      decodePosition(0),
      decodeSamples(0),

      sample_rate(0),
      channels(0),

      source_eof(false),
      decoder_eof(false),

      block_left(nullptr),
      block_right(nullptr),
      block_offset(0)
{
    memset(
        &frameInfo,
        0,
        sizeof(frameInfo)
    );

    if (!inputBuffer.begin()) {

        ESP_LOGE(
            TAG,
            "AudioInputBuffer initialization failed"
        );
    }

    decoder = MP3InitDecoder();

    if (decoder == nullptr) {

        ESP_LOGE(
            TAG,
            "MP3InitDecoder() failed"
        );
    }
}


// ============================================================================
// Destructor
// ============================================================================

AudioPlayMp3::~AudioPlayMp3()
{
    stop();

    if (decoder != nullptr) {

        MP3FreeDecoder(decoder);
        decoder = nullptr;
    }

    inputBuffer.end();
}


// ============================================================================
// Play
// ============================================================================

bool AudioPlayMp3::play(
    AudioSource &newSource
)
{
    return startPlayback(
        newSource,
        false
    );
}


// ============================================================================
// Start playback
// ============================================================================

bool AudioPlayMp3::startPlayback(
    AudioSource &newSource,
    bool takeOwnership
)
{
    stop();

    if (decoder == nullptr) {

        ESP_LOGE(
            TAG,
            "Decoder not initialized"
        );

        return false;
    }

    if (!newSource.isOpen()) {

        ESP_LOGE(
            TAG,
            "Audio source is not open"
        );

        return false;
    }

    source = &newSource;
    own_source = takeOwnership;

    source_eof = false;
    decoder_eof = false;

    sample_rate = 0;
    channels = 0;

    decodePosition = 0;
    decodeSamples = 0;

    block_left = nullptr;
    block_right = nullptr;
    block_offset = 0;

    memset(
        &frameInfo,
        0,
        sizeof(frameInfo)
    );

    g_decodeCalls = 0;
    g_decodedFrames = 0;
    g_syncSearches = 0;
    g_txBlocks = 0;

    /*
     * Der Decoder wird für einen neuen Stream zurückgesetzt.
     */
    resetDecoder();

    state = PLAYING;

    ESP_LOGI(
        TAG,
        "MP3 playback started"
    );

    return true;
}


// ============================================================================
// Reset decoder
// ============================================================================

void AudioPlayMp3::resetDecoder()
{
    if (decoder != nullptr) {

        MP3FreeDecoder(decoder);
    }

    decoder = MP3InitDecoder();
}


// ============================================================================
// Stop
// ============================================================================

void AudioPlayMp3::stop()
{
    if (state == STOPPED) {

        releaseBlocks();
        closeSource();

        return;
    }

    state = STOPPED;

    releaseBlocks();
    closeSource();

    source_eof = false;
    decoder_eof = false;

    decodePosition = 0;
    decodeSamples = 0;

    sample_rate = 0;
    channels = 0;
}


// ============================================================================
// Close source
// ============================================================================

void AudioPlayMp3::closeSource()
{
    if (source == nullptr) {
        return;
    }

    if (own_source) {

        source->close();
    }

    source = nullptr;
    own_source = false;
}


// ============================================================================
// Status
// ============================================================================

bool AudioPlayMp3::isPlaying() const
{
    return state == PLAYING;
}


bool AudioPlayMp3::isStopped() const
{
    return state == STOPPED;
}


// ============================================================================
// Fill input buffer
// ============================================================================

AudioSourceStatus AudioPlayMp3::fillInput()
{
    if (source == nullptr) {

        ESP_LOGE(
            TAG,
            "fillInput(): source == nullptr"
        );

        return AudioSourceStatus::ERROR;
    }

    const AudioSourceStatus status =
        inputBuffer.fill(
            *source
        );

    static uint32_t fillCalls = 0;

    if (fillCalls < 30 ||
        status != AudioSourceStatus::ERROR)
    {
        ESP_LOGI(
            TAG,
            "fillInput #%lu: status=%d source_eof=%d",
            static_cast<unsigned long>(fillCalls),
            static_cast<int>(status),
            source_eof ? 1 : 0
        );
    }

    fillCalls++;

    return status;
}


// ============================================================================
// Decode next MP3 frame
// ============================================================================

bool AudioPlayMp3::decodeNextFrame()
{
    while (true) {

        size_t available = 0;

        const uint8_t *data =
            inputBuffer.acquireRead(
                available
            );

        if (g_decodeCalls < 30) {
            ESP_LOGI(
                TAG,
                "acquireRead: available=%u data=%p source_eof=%d",
                static_cast<unsigned>(available),
                static_cast<const void *>(data),
                source_eof ? 1 : 0
            );
        }


        // --------------------------------------------------------------------
        // No input currently available
        // --------------------------------------------------------------------

        if (data == nullptr ||
            available == 0)
        {
            if (source_eof) {

                decoder_eof = true;

                return false;
            }

            const AudioSourceStatus status =
                fillInput();

            if (status ==
                AudioSourceStatus::ERROR)
            {
                ESP_LOGE(
                    TAG,
                    "Audio source error"
                );

                decoder_eof = true;

                return false;
            }

            if (status ==
                AudioSourceStatus::END_OF_STREAM)
            {
                source_eof = true;
            }

            continue;
        }


        // --------------------------------------------------------------------
        // Find MP3 synchronization word
        // --------------------------------------------------------------------

        const int syncOffset =
            MP3FindSyncWord(
                const_cast<unsigned char *>(data),
                static_cast<int>(available)
            );

        if (g_syncSearches < 30) {
            ESP_LOGI(
                TAG,
                "SYNC #%lu: available=%u offset=%d first=%02X %02X %02X %02X",
                static_cast<unsigned long>(g_syncSearches),
                static_cast<unsigned>(available),
                syncOffset,
                available > 0 ? data[0] : 0,
                available > 1 ? data[1] : 0,
                available > 2 ? data[2] : 0,
                available > 3 ? data[3] : 0
            );
        }
        g_syncSearches++;


        // --------------------------------------------------------------------
        // No sync word in current region
        // --------------------------------------------------------------------

        if (syncOffset < 0) {

            /*
             * Es befindet sich in dieser zusammenhängenden Region
             * kein MP3-Frame-Start.
             *
             * Die komplette Region kann verworfen werden.
             *
             * Wichtig:
             * Wir verwerfen NICHT byteweise.
             */
            inputBuffer.releaseRead(
                available
            );

            if (source_eof) {

                decoder_eof = true;

                return false;
            }

            const AudioSourceStatus status =
                fillInput();

            if (status ==
                AudioSourceStatus::ERROR)
            {
                ESP_LOGE(
                    TAG,
                    "Audio source error while searching for MP3 sync"
                );

                decoder_eof = true;

                return false;
            }

            if (status ==
                AudioSourceStatus::END_OF_STREAM)
            {
                source_eof = true;
            }

            continue;
        }


        // --------------------------------------------------------------------
        // Skip data before MP3 frame
        // --------------------------------------------------------------------

        if (syncOffset > 0) {

            inputBuffer.releaseRead(
                static_cast<size_t>(
                    syncOffset
                )
            );

            /*
             * Jetzt zeigt acquireRead() auf den MP3-Frame.
             */
            continue;
        }


        // --------------------------------------------------------------------
        // MP3 frame starts at current read pointer
        // --------------------------------------------------------------------

        data =
            inputBuffer.acquireRead(
                available
            );

        if (data == nullptr ||
            available == 0)
        {
            continue;
        }

        if (g_decodeCalls < 30) {
            ESP_LOGI(
                TAG,
                "Frame input: available=%u header=%02X %02X %02X %02X",
                static_cast<unsigned>(available),
                available > 0 ? data[0] : 0,
                available > 1 ? data[1] : 0,
                available > 2 ? data[2] : 0,
                available > 3 ? data[3] : 0
            );
        }


        unsigned char *decoderPtr =
            const_cast<unsigned char *>(
                data
            );

        int bytesLeft =
            static_cast<int>(
                available
            );


        if (g_decodeCalls < 30) {
            ESP_LOGI(
                TAG,
                "MP3Decode #%lu: available=%u bytesLeft(before)=%d",
                static_cast<unsigned long>(g_decodeCalls),
                static_cast<unsigned>(available),
                bytesLeft
            );
        }

        const int result =
            MP3Decode(
                decoder,
                &decoderPtr,
                &bytesLeft,
                decodeBuffer,
                0
            );


        /*
         * MP3Decode() verändert den Input-Pointer und bytesLeft.
         *
         * Damit können wir feststellen, wie viele Bytes tatsächlich
         * aus unserem Input-Bereich verarbeitet wurden.
         */
        const size_t consumed =
            available -
            static_cast<size_t>(
                bytesLeft
            );

        if (g_decodeCalls < 30) {
            ESP_LOGI(
                TAG,
                "MP3Decode #%lu: result=%d bytesLeft(after)=%d consumed=%u decoderPtr=%p",
                static_cast<unsigned long>(g_decodeCalls),
                result,
                bytesLeft,
                static_cast<unsigned>(consumed),
                static_cast<void *>(decoderPtr)
            );
        }

        g_decodeCalls++;


        if (consumed > 0) {

            inputBuffer.releaseRead(
                consumed
            );
        }


        // --------------------------------------------------------------------
        // Successful decode
        // --------------------------------------------------------------------

        if (result ==
            ERR_MP3_NONE)
        {
            MP3GetLastFrameInfo(
                decoder,
                &frameInfo
            );

            sample_rate =
                static_cast<uint32_t>(
                    frameInfo.samprate
                );

            channels =
                static_cast<uint16_t>(
                    frameInfo.nChans
                );

            decodePosition = 0;

            decodeSamples =
                static_cast<size_t>(
                    frameInfo.outputSamps
                );

            int16_t pcmMin = 32767;
            int16_t pcmMax = -32768;
            uint32_t pcmNonZero = 0;

            for (size_t i = 0; i < decodeSamples; ++i) {
                const int16_t sample = decodeBuffer[i];

                if (sample < pcmMin) pcmMin = sample;
                if (sample > pcmMax) pcmMax = sample;
                if (sample != 0) pcmNonZero++;
            }

            if (g_decodedFrames < 20 ||
                (g_decodedFrames % 50) == 0)
            {
                ESP_LOGI(
                    TAG,
                    "DECODE frame=%lu consumed=%u bytesLeft=%d "
                    "rate=%d ch=%d bits=%d samples=%d "
                    "PCM[min=%d max=%d nonZero=%lu]",
                    static_cast<unsigned long>(g_decodedFrames),
                    static_cast<unsigned>(consumed),
                    bytesLeft,
                    frameInfo.samprate,
                    frameInfo.nChans,
                    frameInfo.bitsPerSample,
                    frameInfo.outputSamps,
                    pcmMin,
                    pcmMax,
                    static_cast<unsigned long>(pcmNonZero)
                );

                ESP_LOGI(
                    TAG,
                    "PCM first: %d %d %d %d %d %d %d %d",
                    decodeBuffer[0], decodeBuffer[1],
                    decodeBuffer[2], decodeBuffer[3],
                    decodeBuffer[4], decodeBuffer[5],
                    decodeBuffer[6], decodeBuffer[7]
                );

                if (decodeSamples >= 4) {
                    ESP_LOGI(
                        TAG,
                        "PCM last: %d %d %d %d",
                        decodeBuffer[decodeSamples - 4],
                        decodeBuffer[decodeSamples - 3],
                        decodeBuffer[decodeSamples - 2],
                        decodeBuffer[decodeSamples - 1]
                    );
                }
            }

            g_decodedFrames++;


            ESP_LOGI(
                TAG,
                "MP3 frame: %lu Hz, %u ch, %u bit, %u samples",
                static_cast<unsigned long>(
                    frameInfo.samprate
                ),
                static_cast<unsigned>(
                    frameInfo.nChans
                ),
                static_cast<unsigned>(
                    frameInfo.bitsPerSample
                ),
                static_cast<unsigned>(
                    frameInfo.outputSamps
                )
            );


            return true;
        }


        // --------------------------------------------------------------------
        // Main-data underflow
        // --------------------------------------------------------------------

        if (result ==
            ERR_MP3_MAINDATA_UNDERFLOW)
        {
            /*
             * Der Decoder hat den aktuellen MP3-Frame bereits verarbeitet
             * und in sein internes Main-Data-/Bit-Reservoir übernommen.
             *
             * Der Frame kann aber noch nicht dekodiert werden, weil das
             * Bit-Reservoir noch nicht genügend Daten enthält.
             *
             * WICHTIG:
             *
             * MP3Decode() hat den Input bereits konsumiert.
             * Deshalb hier KEIN releaseRead(1)!
             *
             * Der nächste MP3-Frame muss verarbeitet werden, damit der
             * Decoder das erforderliche Main-Data-Reservoir aufbauen kann.
             */

            ESP_LOGI(
                TAG,
                "MP3 main-data underflow: consumed=%u bytesLeft=%d source_eof=%d",
                static_cast<unsigned>(consumed),
                bytesLeft,
                source_eof ? 1 : 0
            );


            if (source_eof) {

                ESP_LOGE(
                    TAG,
                    "MP3 main-data underflow at end of stream"
                );

                decoder_eof = true;

                return false;
            }


            const AudioSourceStatus status =
                fillInput();


            if (status ==
                AudioSourceStatus::ERROR)
            {
                ESP_LOGE(
                    TAG,
                    "Audio source error after MP3 main-data underflow"
                );

                decoder_eof = true;

                return false;
            }


            if (status ==
                AudioSourceStatus::END_OF_STREAM)
            {
                source_eof = true;
            }


            continue;
        }


        // --------------------------------------------------------------------
        // More input required
        // --------------------------------------------------------------------

        if (result ==
            ERR_MP3_INDATA_UNDERFLOW)
        {
            /*
             * Der Frame beginnt korrekt, ist aber noch nicht vollständig
             * im Input-Buffer.
             *
             * Deshalb dürfen wir den Frame-Anfang NICHT verwerfen.
             *
             * Wenn MP3Decode() nichts verbraucht hat, bleibt der komplette
             * Frame im InputBuffer erhalten und kann nach dem Nachladen
             * erneut verarbeitet werden.
             */
            if (consumed == 0) {

                if (source_eof) {

                    ESP_LOGE(
                        TAG,
                        "Unexpected end of MP3 stream"
                    );

                    decoder_eof = true;

                    return false;
                }


                const AudioSourceStatus status =
                    fillInput();


                if (status ==
                    AudioSourceStatus::ERROR)
                {
                    ESP_LOGE(
                        TAG,
                        "Audio source error while completing MP3 frame"
                    );

                    decoder_eof = true;

                    return false;
                }


                if (status ==
                    AudioSourceStatus::END_OF_STREAM)
                {
                    source_eof = true;
                }


                continue;
            }


            /*
             * Falls der Decoder trotz UNDERFLOW bereits Daten verbraucht
             * hat, wurden diese Daten bereits mit releaseRead() aus dem
             * InputBuffer entfernt.
             *
             * Der Decoder kann nun mit weiteren Daten fortfahren.
             */
            if (source_eof) {

                ESP_LOGE(
                    TAG,
                    "Unexpected end of MP3 frame"
                );

                decoder_eof = true;

                return false;
            }


            const AudioSourceStatus status =
                fillInput();


            if (status ==
                AudioSourceStatus::ERROR)
            {
                ESP_LOGE(
                    TAG,
                    "Audio source error while refilling MP3 input"
                );

                decoder_eof = true;

                return false;
            }


            if (status ==
                AudioSourceStatus::END_OF_STREAM)
            {
                source_eof = true;
            }


            continue;
        }


        // --------------------------------------------------------------------
        // Decoder error
        // --------------------------------------------------------------------

        ESP_LOGE(
            TAG,
            "MP3Decode() failed: result=%d available=%u bytesLeft=%d consumed=%u source_eof=%d",
            result,
            static_cast<unsigned>(available),
            bytesLeft,
            static_cast<unsigned>(consumed),
            source_eof ? 1 : 0
        );


        /*
         * Bei einem echten Decoderfehler versuchen wir, einen Byte-Schritt
         * weiterzugehen und anschließend erneut nach einem gültigen MP3
         * Sync-Wort zu suchen.
         */
        if (available > 0) {

            inputBuffer.releaseRead(
                1
            );

            continue;
        }


        if (source_eof) {

            decoder_eof = true;

            return false;
        }
    }
}


// ============================================================================
// Output next decoded sample
// ============================================================================

bool AudioPlayMp3::outputNextSample(
    int16_t &left,
    int16_t &right
)
{
    if (decodePosition >=
        decodeSamples)
    {
        return false;
    }


    if (channels == 1) {

        const int16_t sample =
            decodeBuffer[
                decodePosition
            ];

        left = sample;
        right = sample;

        decodePosition++;

        return true;
    }


    if (decodePosition + 1 >=
        decodeSamples)
    {
        return false;
    }


    left =
        decodeBuffer[
            decodePosition
        ];

    right =
        decodeBuffer[
            decodePosition + 1
        ];

    decodePosition += 2;

    return true;
}


// ============================================================================
// Audio update
// ============================================================================

void AudioPlayMp3::update()
{
    if (state != PLAYING) {
        return;
    }


    // ------------------------------------------------------------------------
    // Need decoded data
    // ------------------------------------------------------------------------

    if (decodePosition >=
        decodeSamples)
    {
        if (decoder_eof) {

            finishPlayback();

            return;
        }


        if (!decodeNextFrame()) {

            if (decoder_eof) {

                finishPlayback();
            }

            return;
        }
    }


    // ------------------------------------------------------------------------
    // Allocate audio blocks
    // ------------------------------------------------------------------------

if (block_left == nullptr) {

    block_left = allocate();

    if (block_left == nullptr) {

        ESP_LOGE(
            TAG,
            "UPDATE: allocate(block_left) FAILED "
            "decodePosition=%u decodeSamples=%u "
            "block_offset=%u AudioMemory=%u",
            static_cast<unsigned>(decodePosition),
            static_cast<unsigned>(decodeSamples),
            static_cast<unsigned>(block_offset),
            static_cast<unsigned>(AUDIO_BLOCK_SAMPLES)
        );

        return;
    }
}


if (block_right == nullptr) {

    block_right = allocate();

    if (block_right == nullptr) {

        ESP_LOGE(
            TAG,
            "UPDATE: allocate(block_right) FAILED "
            "decodePosition=%u decodeSamples=%u "
            "block_offset=%u",
            static_cast<unsigned>(decodePosition),
            static_cast<unsigned>(decodeSamples),
            static_cast<unsigned>(block_offset)
        );

        release(block_left);

        block_left = nullptr;

        return;
    }
}


    // ------------------------------------------------------------------------
    // Fill audio block
    // ------------------------------------------------------------------------

    while (
        block_offset <
        AUDIO_BLOCK_SAMPLES
    )
    {
        int16_t left;
        int16_t right;


        if (!outputNextSample(
                left,
                right))
        {
            /*
             * Aktueller MP3-Frame ist fertig.
             *
             * Nächsten Frame dekodieren.
             */
            if (!decodeNextFrame()) {

                if (decoder_eof) {
                    break;
                }
ESP_LOGI(TAG,"Frame ist fertig.");
                return;
            }

            continue;
        }


        block_left->data[
            block_offset
        ] = left;

        block_right->data[
            block_offset
        ] = right;

        block_offset++;
    }


    // ------------------------------------------------------------------------
    // Transmit complete block
    // ------------------------------------------------------------------------

    if (block_offset >=
        AUDIO_BLOCK_SAMPLES)
    {
        if (g_txBlocks < 20 ||
            (g_txBlocks % 100) == 0)
        {
            int16_t leftMin = 32767;
            int16_t leftMax = -32768;
            int16_t rightMin = 32767;
            int16_t rightMax = -32768;
            uint32_t leftNonZero = 0;
            uint32_t rightNonZero = 0;

            for (size_t i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) {
                const int16_t l = block_left->data[i];
                const int16_t r = block_right->data[i];

                if (l < leftMin) leftMin = l;
                if (l > leftMax) leftMax = l;
                if (r < rightMin) rightMin = r;
                if (r > rightMax) rightMax = r;
                if (l != 0) leftNonZero++;
                if (r != 0) rightNonZero++;
            }

            ESP_LOGI(
                TAG,
                "TX block %lu: L[min=%d max=%d nz=%lu] "
                "R[min=%d max=%d nz=%lu] samples=%u",
                static_cast<unsigned long>(g_txBlocks),
                leftMin,
                leftMax,
                static_cast<unsigned long>(leftNonZero),
                rightMin,
                rightMax,
                static_cast<unsigned long>(rightNonZero),
                static_cast<unsigned>(AUDIO_BLOCK_SAMPLES)
            );

            ESP_LOGI(
                TAG,
                "TX first: L=%d,%d,%d,%d R=%d,%d,%d,%d",
                block_left->data[0], block_left->data[1],
                block_left->data[2], block_left->data[3],
                block_right->data[0], block_right->data[1],
                block_right->data[2], block_right->data[3]
            );
        }

        g_txBlocks++;


        transmit(
            block_left,
            0
        );
        release(block_left);

        transmit(
            block_right,
            1
        );
        release(block_right);

        block_left = nullptr;
        block_right = nullptr;

        block_offset = 0;
    }
}


// ============================================================================
// Release blocks
// ============================================================================

void AudioPlayMp3::releaseBlocks()
{
    if (block_left != nullptr) {

        release(
            block_left
        );

        block_left = nullptr;
    }


    if (block_right != nullptr) {

        release(
            block_right
        );

        block_right = nullptr;
    }


    block_offset = 0;
}


// ============================================================================
// Finish playback
// ============================================================================

void AudioPlayMp3::finishPlayback()
{
    releaseBlocks();

    state = STOPPED;

    ESP_LOGI(
        TAG,
        "MP3 playback finished"
    );
}
