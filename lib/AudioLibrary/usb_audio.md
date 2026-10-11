# USB Audio nodes

`AudioInputUSB` receives host playback (PC to ESP32), while `AudioOutputUSB` sends the graph output to the host (ESP32 to PC), for example as a microphone/capture device.

## Initial backend

- Target: ESP32-S3 using the Arduino-ESP32 `USBAudioCard` API and TinyUSB.
- Initial format: signed interleaved 16-bit PCM at the current `AudioStream` sample rate; stereo by default.
- `AudioInputUSB` presents channel 0 as left and channel 1 as right.
- `AudioOutputUSB` accepts channel 0 as left and channel 1 as right.
- The public channel-count API is configurable up to `AUDIO_USB_MAX_CHANNELS` (default 8), but the initial `USBAudioCard` backend currently accepts only mono/stereo. Larger counts fail initialization instead of being silently truncated.
- Construct the USB nodes before `AudioMemory()`. USB descriptors are registered once, before the device is started.
- The library starts the TinyUSB device when the first USB node initializes. Do not call `USB.begin()` before the audio nodes have registered their interfaces.

## FNK0104B / PlatformIO

Use the PioArduino stable PlatformIO platform for the FNK0104B environment. This supplies a recent Arduino-ESP32 3.x core with the `USBAudioCard` API (introduced in 3.3.8). Use USB-OTG/TinyUSB (`ARDUINO_USB_MODE=0`) and leave `ARDUINO_USB_CDC_ON_BOOT=1` so the sketch can keep using `Serial.println()`. CDC is not an audio-library feature; it is registered by Arduino-ESP32.

**Unverified compatibility point:** Espressif's stock `AudioCard` example currently requires CDC-on-boot to be disabled. The library configures CDC and audio as separate TinyUSB interfaces before calling `USB.begin()`, but simultaneous CDC + UAC operation must be confirmed on the FNK0104B with the selected Arduino-ESP32 core. This implementation does not claim that composite mode has already passed a hardware test.

## Sample rate

The USB descriptor advertises one sample rate, matching `AudioStream::sampleRate()` when the device is initialized. Call `usbInput.setSampleRate(48000)` or `usbOutput.setSampleRate(48000)` before `AudioMemory()` if a 48 kHz graph is desired. Changing the rate after USB setup is rejected; host-driven runtime sample-rate changes are not implemented yet.
