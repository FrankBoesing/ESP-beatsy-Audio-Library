Compatible to Teensy audio Library


Optimized for ESP, in a 2nd step ESP-S3


Work in Progress.

## Runtime tasks

The application uses the following tasks. Stack sizes below are the values
passed to FreeRTOS by this ESP-IDF build. Priorities are expressed relative to
`configMAX_PRIORITIES` so they remain accurate if the SDK configuration changes.

| Task | Created by | Priority | Stack | Core affinity | Responsibility |
| --- | --- | --- | --- | --- | --- |
| `loopTask` | Arduino framework | 1 | 8192 bytes by default; configurable | `ARDUINO_RUNNING_CORE` | Runs `setup()` once and `loop()` repeatedly. The stream example waits for its startup prebuffer and prints diagnostics here. |
| `AudioTask` | `AudioStream::update_setup()` | `configMAX_PRIORITIES - 2` | 4096 bytes | `AUDIO_PROCESSING_CORE` (default 1) | Processes active `AudioStream` objects, including mixers and effects, after a notification. With I2S active, the I2S DMA callback provides the audio clock and notifies this task. |
| `AudioDecoder` | `AudioDecoderStream::startDecoderTask()` | `configMAX_PRIORITIES - 3` | 8192 bytes | `AUDIO_DECODER_CORE` (default 0) | Decodes MP3/AAC frames and fills the two alternating PCM buffers in internal RAM. |
| `AudioI2STx` | `AudioOutputI2S::beginInternal()` | `configMAX_PRIORITIES - 2` | 4096 bytes | `AUDIO_PROCESSING_CORE` (default 1) | Takes stereo blocks from the I2S queue, converts samples to 32-bit slots, and writes them to the I2S driver. |
| `AudioStreamRx` | `AudioSourceStream::open()` | `configMAX_PRIORITIES - 5` | 8192 bytes | Unpinned | Reads available network bytes into a 2-KiB temporary chunk and copies them into the stream ring buffer in PSRAM. Exists only for stream sources. |

The ESP-IDF also owns the Wi-Fi, TCP/IP, event-loop, timer-service, and FreeRTOS
idle tasks. Their priorities and core assignments are controlled by the
framework/SDK configuration, not by this library. The `esp_timer` service
delivers the software audio clock when software-clock mode is used; active I2S
output uses the I2S DMA callback instead. The DMA callback is an ISR, not a task.

## Stream data path

For network playback, bytes pass through these stages:

1. `AudioStreamRx` reads up to 2 KiB from the HTTP stream into a temporary
    task-local chunk.
2. The producer copies that chunk into the 128-KiB PSRAM ring buffer.
3. `AudioSourceStream::read()` copies available ring-buffer bytes into the
    MP3 decoder's 2-KiB `_input` buffer in internal RAM, then releases that ring
    space for the producer.
4. `AudioDecoder` runs `MP3Decode()` from `_input` and writes PCM into one of
    the two internal-RAM PCM buffers.
5. `AudioTask` consumes ready PCM through the audio graph and queues blocks for
    `AudioI2STx`.
6. `AudioI2STx` writes the blocks to the I2S driver; DMA clocks them to the
    codec.

The 128-KiB PSRAM ring is only used by streaming. File playback continues to
use the decoder's existing 2-KiB input buffer and does not create `AudioStreamRx`.

Working example 1:
~~~ 
#include <Arduino.h>
#include <SD_MMC.h>

#include "AudioStream.h"
#include "play_sd_wav.h"
#include "output_i2s.h"
#include "control_es8388.h"


AudioPlaySdWav wav;

AudioOutputI2S i2s({
    27,  // BCLK
    25,  // WS / LRCLK
    26,  // DOUT
    0    // MCLK
});

AudioControlES8388 codec;

AudioConnection patchCord1(wav, 0, i2s, 0);
AudioConnection patchCord2(wav, 0, i2s, 1);


void setup()
{
    Serial.begin(115200);
    delay(1000);

    Serial.println();
    Serial.println("================================");
    Serial.println(" AUDIO PLAY SD WAV TEST");
    Serial.println("================================");

    AudioMemory(12);

    Serial.println("Initializing ES8388...");

    if (!codec.enable()) {
        Serial.println("ERROR: ES8388 initialization failed");

        while (1) {
            delay(1000);
        }
    }

    Serial.println("ES8388 initialized");

    Serial.println("Initializing SD_MMC...");

    // ESP32 Audio Kit V2.2:
    // first test in 1-bit mode.
    if (!SD_MMC.begin("/sdcard", true)) {

        Serial.println(
            "ERROR: SD_MMC initialization failed"
        );

        while (1) {
            delay(1000);
        }
    }

    Serial.println("SD_MMC initialized");

    Serial.println("Opening /test.wav...");

    if (!wav.play("/test.wav")) {

        Serial.println(
            "ERROR: WAV playback could not be started"
        );

        while (1) {
            delay(1000);
        }
    }

    Serial.println("WAV playback started");
}


void loop()
{
    static uint32_t lastStatus = 0;

    if (millis() - lastStatus >= 1000) {

        lastStatus = millis();

        Serial.print("Playing: ");
        Serial.print(wav.isPlaying());

        Serial.print("  Position: ");
        Serial.print(wav.positionMillis());

        Serial.print(" ms / ");
        Serial.print(wav.lengthMillis());

        Serial.println(" ms");
    }
}
~~~ 
