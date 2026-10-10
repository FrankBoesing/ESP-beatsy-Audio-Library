#ifndef analyze_print_h_
#define analyze_print_h_

#include <Arduino.h>
#include <AudioStream.h>

class AudioAnalyzePrint : public AudioStream {
public:
    AudioAnalyzePrint(void);

    void update(void) override;

    // Teensy-compatible API; currently not implemented.
    void name(const char *str) {
        (void)str;
    }

    void trigger(void);
    void trigger(float level, int edge);

    void delay(uint32_t num) {
        (void)num;
    }

    void length(uint32_t num) {
        (void)num;
    }

private:
    audio_block_t *inputQueueArray[1];
};

#endif
