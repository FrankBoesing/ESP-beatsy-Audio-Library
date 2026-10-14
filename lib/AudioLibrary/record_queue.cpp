/* Audio Library for Teensy 3.X
 * Copyright (c) 2014, Paul Stoffregen, paul@pjrc.com
 *
 * Development of this audio library was funded by PJRC.COM, LLC by sales of
 * Teensy and Audio Adaptor boards.  Please support PJRC's efforts to develop
 * open source software by purchasing Teensy or other PJRC products.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice, development funding notice, and this permission
 * notice shall be included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include <Arduino.h>
#include "record_queue.h"

// AudioRecordQueue's public API may be called from an application task while
// update() runs on the audio task. Keep queue state protected, but never call
// AudioStream::release() while holding this lock (release uses audio_mux).
static portMUX_TYPE record_queue_mux = portMUX_INITIALIZER_UNLOCKED;

void AudioRecordQueue::begin(void)
{
	clear();

	portENTER_CRITICAL(&record_queue_mux);
	enabled = 1;
	portEXIT_CRITICAL(&record_queue_mux);
}

void AudioRecordQueue::end(void)
{
	portENTER_CRITICAL(&record_queue_mux);
	enabled = 0;
	portEXIT_CRITICAL(&record_queue_mux);
}

int AudioRecordQueue::available(void)
{
	uint32_t h, t;

	portENTER_CRITICAL(&record_queue_mux);
	h = head;
	t = tail;
	portEXIT_CRITICAL(&record_queue_mux);

	if (h >= t) return h - t;
	return max_buffers + h - t;
}

void AudioRecordQueue::clear(void)
{
	// At most max_buffers blocks can be owned: max_buffers - 1 queued blocks
	// plus the block currently handed to the caller by readBuffer().
	audio_block_t *pending[max_buffers];
	unsigned int count = 0;

	portENTER_CRITICAL(&record_queue_mux);

	if (userblock != NULL) {
		pending[count++] = userblock;
		userblock = NULL;
	}

	uint8_t t = tail;
	while (t != head) {
		if (++t >= max_buffers) t = 0;
		if (queue[t] != NULL) {
			pending[count++] = queue[t];
			queue[t] = NULL;
		}
	}
	tail = head;

	portEXIT_CRITICAL(&record_queue_mux);

	// AudioStream::release() uses a separate critical section.
	for (unsigned int i = 0; i < count; ++i) {
		release(pending[i]);
	}
}

int16_t * AudioRecordQueue::readBuffer(void)
{
	int16_t *data = NULL;

	portENTER_CRITICAL(&record_queue_mux);

	if (userblock == NULL && tail != head) {
		uint8_t t = tail + 1;
		if (t >= max_buffers) t = 0;

		userblock = queue[t];
		queue[t] = NULL;
		tail = t;

		if (userblock != NULL) {
			data = userblock->data;
		}
	}

	portEXIT_CRITICAL(&record_queue_mux);
	return data;
}

void AudioRecordQueue::freeBuffer(void)
{
	audio_block_t *block = NULL;

	portENTER_CRITICAL(&record_queue_mux);
	block = userblock;
	userblock = NULL;
	portEXIT_CRITICAL(&record_queue_mux);

	if (block != NULL) {
		release(block);
	}
}

void AudioRecordQueue::update(void)
{
	audio_block_t *block = receiveReadOnly();
	if (block == NULL) return;

	portENTER_CRITICAL(&record_queue_mux);

	if (enabled) {
		uint8_t h = head + 1;
		if (h >= max_buffers) h = 0;

		if (h != tail) {
			queue[h] = block;
			head = h;
			block = NULL;
		}
	}

	portEXIT_CRITICAL(&record_queue_mux);

	// Disabled queue or full queue: drop this block without blocking audio.
	if (block != NULL) {
		release(block);
	}
}
