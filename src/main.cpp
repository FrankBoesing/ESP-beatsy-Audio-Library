#include <Arduino.h>
#include <SD_MMC.h>

#include "AudioSourceFile.h"
#include "AudioInputBuffer.h"


static const char *TEST_FILE = "/test.mp3";

AudioSourceFile source;

AudioInputBuffer inputBuffer(32 * 1024);


void printBufferState(const char *label)
{
    size_t readable = 0;

    inputBuffer.acquireRead(readable);

    Serial.printf(
        "%-20s readable=%6u  free=%6u  capacity=%6u  PSRAM=%s\n",
        label,
        (unsigned)readable,
        (unsigned)inputBuffer.freeSpace(),
        (unsigned)inputBuffer.capacity(),
        inputBuffer.usingPSRAM() ? "yes" : "no"
    );
}


void setup()
{
    Serial.begin(115200);
    delay(1000);

    Serial.println();
    Serial.println("======================================");
    Serial.println(" AudioInputBuffer SD_MMC TEST");
    Serial.println("======================================");


    // ------------------------------------------------------------------------
    // PSRAM
    // ------------------------------------------------------------------------

#if defined(ARDUINO_ARCH_ESP32)

    Serial.printf(
        "PSRAM found:      %s\n",
        psramFound() ? "yes" : "no"
    );

    Serial.printf(
        "PSRAM size:       %u bytes\n",
        (unsigned)ESP.getPsramSize()
    );

    Serial.printf(
        "Free PSRAM:       %u bytes\n",
        (unsigned)ESP.getFreePsram()
    );

#endif


    // ------------------------------------------------------------------------
    // SD_MMC
    // ------------------------------------------------------------------------

    Serial.println();
    Serial.println("Initializing SD_MMC...");

    if (!SD_MMC.begin("/sdcard", true)) {

        Serial.println(
            "ERROR: SD_MMC initialization failed"
        );

        while (true) {
            delay(1000);
        }
    }

    Serial.println("SD_MMC initialized");


    // ------------------------------------------------------------------------
    // Source
    // ------------------------------------------------------------------------

    Serial.printf(
        "Opening %s...\n",
        TEST_FILE
    );

    if (!source.open(
            SD_MMC,
            TEST_FILE))
    {
        Serial.println(
            "ERROR: Could not open test file"
        );

        while (true) {
            delay(1000);
        }
    }

    Serial.printf(
        "Source opened, size=%llu bytes\n",
        (unsigned long long)source.size()
    );


    // ------------------------------------------------------------------------
    // Input buffer
    // ------------------------------------------------------------------------

    Serial.println();
    Serial.println("Initializing AudioInputBuffer...");

    if (!inputBuffer.begin()) {

        Serial.println(
            "ERROR: AudioInputBuffer allocation failed"
        );

        while (true) {
            delay(1000);
        }
    }

    Serial.println("AudioInputBuffer initialized");

    printBufferState("Initial");


    // ========================================================================
    // TEST 1
    // ========================================================================

    Serial.println();
    Serial.println("--------------------------------------");
    Serial.println("TEST 1: initial fill");
    Serial.println("--------------------------------------");

    AudioSourceStatus status =
        inputBuffer.fill(source);

    Serial.printf(
        "fill() status: %d\n",
        (int)status
    );

    printBufferState("After fill");


    size_t readable = 0;

    const uint8_t *ptr =
        inputBuffer.acquireRead(readable);

    if (ptr == nullptr) {

        Serial.println(
            "ERROR: acquireRead() returned nullptr"
        );

    }
    else {

        Serial.println(
            "acquireRead() returned valid pointer"
        );

        Serial.printf(
            "Readable bytes: %u\n",
            (unsigned)readable
        );

        Serial.print(
            "First 16 bytes:"
        );

        const size_t count =
            min(
                (size_t)16,
                readable
            );

        for (size_t i = 0; i < count; ++i) {

            Serial.printf(
                " %02X",
                ptr[i]
            );
        }

        Serial.println();
    }


    // ========================================================================
    // TEST 2
    // ========================================================================

    Serial.println();
    Serial.println("--------------------------------------");
    Serial.println("TEST 2: releaseRead without memcpy");
    Serial.println("--------------------------------------");

    const size_t consumeSize =
        min(
            (size_t)1024,
            readable
        );

    Serial.printf(
        "Releasing %u bytes...\n",
        (unsigned)consumeSize
    );

    if (!inputBuffer.releaseRead(
            consumeSize))
    {
        Serial.println(
            "ERROR: releaseRead() failed"
        );

    }
    else {

        Serial.println(
            "releaseRead() successful"
        );
    }

    printBufferState("After release");


    // ========================================================================
    // TEST 3
    // ========================================================================

    Serial.println();
    Serial.println("--------------------------------------");
    Serial.println("TEST 3: refill");
    Serial.println("--------------------------------------");

    status =
        inputBuffer.fill(source);

    Serial.printf(
        "fill() status: %d\n",
        (int)status
    );

    printBufferState("After refill");


    // ========================================================================
    // TEST 4
    // ========================================================================

    Serial.println();
    Serial.println("--------------------------------------");
    Serial.println("TEST 4: force wrap-around");
    Serial.println("--------------------------------------");


    /*
     * Nur 4096 Bytes sollen übrig bleiben.
     */
    inputBuffer.acquireRead(readable);

    if (readable > 4096) {

        const size_t amount =
            readable - 4096;

        if (!inputBuffer.releaseRead(
                amount))
        {
            Serial.println(
                "ERROR: release before wrap failed"
            );
        }
    }

    printBufferState("Before wrap fill");


    /*
     * Jetzt sollte der freie Bereich hinter A gefüllt werden.
     */
    status =
        inputBuffer.fill(source);

    Serial.printf(
        "fill #1 -> status=%d\n",
        (int)status
    );

    printBufferState("After wrap fill");


    /*
     * 1024 Bytes konsumieren.
     */
    inputBuffer.acquireRead(readable);

    if (readable >= 1024) {

        inputBuffer.releaseRead(1024);
    }

    printBufferState("After consume 1");


    /*
     * Jetzt erneut füllen.
     *
     * Wenn der Bip-Buffer korrekt funktioniert, kann
     * dieser Fill am Anfang des Buffers eine zweite Region
     * erzeugen.
     */
    status =
        inputBuffer.fill(source);

    Serial.printf(
        "fill #2 -> status=%d\n",
        (int)status
    );

    printBufferState("After fill 2");


    // ========================================================================
    // TEST 5
    // ========================================================================

    Serial.println();
    Serial.println("--------------------------------------");
    Serial.println("TEST 5: region transition");
    Serial.println("--------------------------------------");


    /*
     * Aktuelle Region A schrittweise vollständig verbrauchen.
     *
     * Wenn Region B existiert, muss sie danach automatisch
     * zur neuen Region A werden.
     */
    while (true) {

        inputBuffer.acquireRead(readable);

        if (readable == 0) {
            break;
        }

        Serial.printf(
            "Current read region: %u bytes\n",
            (unsigned)readable
        );

        const size_t consume =
            min(
                readable,
                (size_t)1024
            );

        if (!inputBuffer.releaseRead(
                consume))
        {
            Serial.println(
                "ERROR: releaseRead() failed"
            );

            break;
        }
    }

    printBufferState("After region transition");


    // ========================================================================
    // TEST 6
    // ========================================================================

    Serial.println();
    Serial.println("--------------------------------------");
    Serial.println("TEST 6: acquireWrite / commitWrite");
    Serial.println("--------------------------------------");


    size_t writeLength = 0;

    uint8_t *writePtr =
        inputBuffer.acquireWrite(writeLength);

    if (writePtr == nullptr) {

        Serial.println(
            "No writable region available"
        );

    }
    else {

        Serial.printf(
            "Writable region: %u bytes\n",
            (unsigned)writeLength
        );


        const size_t testBytes =
            min(
                writeLength,
                (size_t)16
            );


        for (size_t i = 0; i < testBytes; ++i) {

            writePtr[i] =
                static_cast<uint8_t>(
                    0xA0 + i
                );
        }


        if (!inputBuffer.commitWrite(
                testBytes))
        {
            Serial.println(
                "ERROR: commitWrite() failed"
            );

        }
        else {

            Serial.printf(
                "Committed %u bytes\n",
                (unsigned)testBytes
            );
        }
    }


    // ========================================================================
    // TEST 7
    // ========================================================================

    Serial.println();
    Serial.println("--------------------------------------");
    Serial.println("TEST 7: pointer stability");
    Serial.println("--------------------------------------");


    inputBuffer.acquireRead(readable);

    const uint8_t *ptr1 =
        inputBuffer.acquireRead(readable);

    Serial.printf(
        "Pointer before fill: %p\n",
        ptr1
    );

    Serial.printf(
        "Readable before fill: %u\n",
        (unsigned)readable
    );


    inputBuffer.fill(source);


    size_t readableAfter = 0;

    const uint8_t *ptr2 =
        inputBuffer.acquireRead(
            readableAfter
        );

    Serial.printf(
        "Pointer after fill:  %p\n",
        ptr2
    );

    Serial.printf(
        "Readable after fill: %u\n",
        (unsigned)readableAfter
    );


    if (ptr1 == ptr2) {

        Serial.println(
            "PASS: read pointer remained stable"
        );

    }
    else {

        Serial.println(
            "NOTE: read pointer changed"
        );
    }


    // ------------------------------------------------------------------------
    // Done
    // ------------------------------------------------------------------------

    Serial.println();
    Serial.println("======================================");
    Serial.println(" TEST COMPLETE");
    Serial.println("======================================");

    printBufferState("Final");
}


void loop()
{
    delay(1000);
}
