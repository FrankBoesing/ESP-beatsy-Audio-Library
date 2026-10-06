/* Audio Library for Teensy 3.X
 * Copyright (c) 2014, Paul Stoffregen, paul@pjrc.com
 *
 * Development of this audio library was funded by PJRC.COM, LLC by sales of
 * Teensy and Audio Adaptor boards.  Please support PJRC's efforts by purchasing
 * Teensy or other PJRC products.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, subject to the following conditions:
 *
 * The above copyright notice, development funding notice, and this permission
 * notice shall be included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 */

#include <Arduino.h>
#include "play_queue.h"
#include "utility/dspinst.h"

static portMUX_TYPE play_queue_mux = portMUX_INITIALIZER_UNLOCKED;


void AudioPlayQueue::setMaxBuffers(uint8_t maxb)
{
    if (maxb < 2)
        maxb = 2;

    if (maxb > MAX_BUFFERS)
        maxb = MAX_BUFFERS;

    audio_block_t *pending[MAX_BUFFERS];
    unsigned int count = 0;

    portENTER_CRITICAL(&play_queue_mux);

    uint8_t index = tail;

    while (index != head) {
        uint8_t next = index + 1;

        if (next >= max_buffers)
            next = 0;

        index = next;
        pending[count++] = queue[index];
    }

    // At least one slot must remain unused so head != tail
    // can distinguish full from empty.
    if (maxb <= count)
        maxb = count + 1;

    memset(queue, 0, sizeof(queue));

    max_buffers = maxb;
    tail = 0;
    head = count;

    for (unsigned int i = 0; i < count; ++i)
        queue[i + 1] = pending[i];

    portEXIT_CRITICAL(&play_queue_mux);
}


bool AudioPlayQueue::available(void)
{
    if (userblock)
        return true;

    userblock = allocate();

    return userblock != NULL;
}


/**
 * Get address of current data buffer, newly allocated if necessary.
 *
 * With behaviour == ORIGINAL this will stall (calling yield()) until an
 * audio block becomes available.
 *
 * With behaviour == NON_STALLING this will never stall and returns NULL
 * if no audio block is available.
 */
int16_t *AudioPlayQueue::getBuffer(void)
{
    if (userblock == NULL) {
        switch (behaviour) {

        default:
            while (1) {
                userblock = allocate();

                if (userblock)
                    break;

                yield();
            }
            break;

        case NON_STALLING:
            userblock = allocate();
            break;
        }
    }

    return userblock ? userblock->data : NULL;
}


/**
 * Queue userblock for later playback in update().
 *
 * Returns:
 *   0 = success
 *   1 = retry required
 */
uint32_t AudioPlayQueue::playBuffer(void)
{
    if (!userblock)
        return 0;

    for (;;) {
        portENTER_CRITICAL(&play_queue_mux);

        uint8_t next = head + 1;

        if (next >= max_buffers)
            next = 0;

        if (tail != next) {
            queue[next] = userblock;
            head = next;
            userblock = NULL;

            portEXIT_CRITICAL(&play_queue_mux);
            return 0;
        }

        portEXIT_CRITICAL(&play_queue_mux);

        if (behaviour == NON_STALLING)
            return 1;

        yield();
    }
}


/**
 * Put one sample into the current buffer.
 *
 * Returns:
 *   0 = success
 *   1 = failed, call again with same data
 */
uint32_t AudioPlayQueue::play(int16_t data)
{
    uint32_t result = 1;
    int16_t *buf = getBuffer();

    do {
        if (buf == NULL)
            break;

        if (uptr >= AUDIO_BLOCK_SAMPLES) {

            if (playBuffer() == 0) {
                uptr = 0;
                buf = getBuffer();
                continue;
            }

        } else {

            buf[uptr++] = data;
            result = 0;

            if (uptr >= AUDIO_BLOCK_SAMPLES &&
                playBuffer() == 0) {

                uptr = 0;
            }
        }

    } while (false);

    return result;
}


/**
 * Put multiple samples into buffer(s).
 *
 * Returns:
 *   0 = success
 *   >0 = number of samples not stored
 */
uint32_t AudioPlayQueue::play(const int16_t *data, uint32_t len)
{
    uint32_t result = len;
    int16_t *buf = getBuffer();

    do {
        if (buf == NULL)
            break;

        if (uptr >= AUDIO_BLOCK_SAMPLES) {

            if (playBuffer() == 0) {
                uptr = 0;
                buf = getBuffer();
                continue;
            }

            break;
        }

        if (len == 0)
            break;

        const uint32_t available_samples =
            AUDIO_BLOCK_SAMPLES - uptr;

        const uint32_t to_copy =
            available_samples < len
                ? available_samples
                : len;

        memcpy(buf + uptr,
               data,
               to_copy * sizeof(int16_t));

        uptr += to_copy;
        data += to_copy;
        len -= to_copy;
        result -= to_copy;

        if (uptr >= AUDIO_BLOCK_SAMPLES) {

            if (playBuffer() == 0) {
                uptr = 0;

                if (len > 0)
                    buf = getBuffer();
            } else {
                break;
            }
        }

    } while (len > 0);

    return result;
}


void AudioPlayQueue::update(void)
{
    audio_block_t *block = NULL;

    portENTER_CRITICAL(&play_queue_mux);

    if (tail != head) {

        uint8_t next_tail = tail + 1;

        if (next_tail >= max_buffers)
            next_tail = 0;

        tail = next_tail;

        block = queue[tail];
        queue[tail] = NULL;
    }

    portEXIT_CRITICAL(&play_queue_mux);

    if (block) {
        transmit(block);
        release(block);
    }
}


void AudioPlayQueue::stop(void)
{
    audio_block_t *pending[MAX_BUFFERS];
    unsigned int count = 0;

    audio_block_t *current = userblock;

    userblock = NULL;
    uptr = 0;

    portENTER_CRITICAL(&play_queue_mux);

    uint8_t index = tail;

    while (index != head) {
        uint8_t next = index + 1;

        if (next >= max_buffers)
            next = 0;

        index = next;

        pending[count++] = queue[index];
        queue[index] = NULL;
    }

    head = 0;
    tail = 0;

    portEXIT_CRITICAL(&play_queue_mux);

    if (current)
        release(current);

    for (unsigned int i = 0; i < count; ++i)
        release(pending[i]);
}
