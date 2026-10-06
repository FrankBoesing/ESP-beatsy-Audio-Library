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
#include "play_queue.h"
#include "utility/dspinst.h"

static portMUX_TYPE play_queue_mux = portMUX_INITIALIZER_UNLOCKED;

void AudioPlayQueue::setMaxBuffers(uint8_t maxb)
{
  if (maxb < 2)
    maxb = 2 ;
  if (maxb > MAX_BUFFERS)
    maxb = MAX_BUFFERS ;

  audio_block_t *pending[MAX_BUFFERS];
  unsigned int count = 0;

  portENTER_CRITICAL(&play_queue_mux);
  uint8_t index = tail;
  while (index != head) {
    if (++index >= max_buffers) index = 0;
    pending[count++] = queue[index];
  }

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
        if (userblock) return true;
        userblock = allocate();
        if (userblock) return true;
        return false;
}


/**
 * Get address of current data buffer, newly allocated if necessary.
 * With behaviour == ORIGINAL this will stall (calling yield()) until an audio block
 * becomes available - there's no real guarantee this will ever happen...
 * With behaviour == NON_STALLING this will never stall, and will conform to the published
 * API by returning NULL if no audio block is available.
 * \return: NULL if buffer not available, else pointer to buffer of AUDIO_BLOCK_SAMPLES of int16_t
 */
int16_t* AudioPlayQueue::getBuffer(void)
{
	if (NULL == userblock) // not got one: try to get one
	{
		switch (behaviour)
		{
			default:
				while (1)
				{
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

	return userblock == NULL
					?NULL
					:userblock->data;
}


/**
 * Queue userblock for later playback in update().
 * If there's no user block in use then we presume success: this means it's
 * safe to keep calling playBuffer() regularly even if we've not been
 * creating audio to be played.
 * \return 0 for success, 1 for re-try required
 */
uint32_t AudioPlayQueue::playBuffer(void)
{
	if (!userblock)
		return 0;

	for (;;) {
		portENTER_CRITICAL(&play_queue_mux);
		uint8_t next = head + 1;
		if (next >= max_buffers) next = 0;

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
 * Put sample to buffer, and queue if buffer full.
 * \return 0: success; 1: failed, data not stored, call again with same data
 */
uint32_t AudioPlayQueue::play(int16_t data)
{
	uint32_t result = 1;
	int16_t* buf = getBuffer();

	do
	{
		if (NULL == buf) // no buffer, failed already
			break;

		if (uptr >= AUDIO_BLOCK_SAMPLES) // buffer is full, we're re-called: try again
		{
			if (0 == playBuffer()) // success emitting old buffer...
			{
				uptr = 0;			// ...start at beginning...
				buf = getBuffer();	// ...of new buffer
				continue;			// loop to check buffer and store the sample
			}
		}
		else // non-full buffer
		{
		  buf [uptr++] = data ;
		  result = 0;
		  if (uptr >= AUDIO_BLOCK_SAMPLES	// buffer is full...
		   && 0 == playBuffer())			// ... try to queue it
			  uptr = 0; // success!
		}
	} while (false);

	return result;
}

/**
 * Put multiple samples to buffer(s), and queue if buffer(s) full.
 * \return 0: success; >0: failed, data not stored, call again with remaining data (return is unused data length)
 */
uint32_t AudioPlayQueue::play(const int16_t *data, uint32_t len)
{
	uint32_t result = len;
	int16_t * buf = getBuffer();

	do
	{
		unsigned int avail_in_userblock = AUDIO_BLOCK_SAMPLES - uptr ;
		unsigned int to_copy = avail_in_userblock > len ? len : avail_in_userblock ;

		if (NULL == buf) // no buffer, failed
			break;

		if (uptr >= AUDIO_BLOCK_SAMPLES) // buffer is full, we're re-called: try again
		{
			if (0 == playBuffer()) // success emitting old buffer...
			{
				uptr = 0;			// ...start at beginning...
				buf = getBuffer();	// ...of new buffer
				continue;			// loop to check buffer and store more samples
			}
		}

		if (0 == len) // nothing left to do
			break;

		// we have a buffer and something to copy to it: do that
		memcpy(buf + uptr, data, to_copy * sizeof(int16_t));
		uptr   += to_copy;
		data   += to_copy;
		len    -= to_copy;
		result -= to_copy;

		if (uptr >= AUDIO_BLOCK_SAMPLES)	// buffer is full...
		{
			if (0 == playBuffer())			// ... try to queue it
			{
				uptr = 0;					// success!
				if (len > 0)				// more to buffer...
					buf = getBuffer();		// ...try to do that
			}
			else
				break;	// queue failed: exit and try again later
		}
	} while (len > 0);

	return result;
}


void AudioPlayQueue::update(void)
{
	audio_block_t *block = NULL;

	portENTER_CRITICAL(&play_queue_mux);
	if (tail != head) {
		if (tail + 1 >= max_buffers)
			tail = 0;
		else
			tail = tail + 1;

		block = queue[tail];
		queue[tail] = nullptr;
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
		if (++index >= max_buffers) index = 0;
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
