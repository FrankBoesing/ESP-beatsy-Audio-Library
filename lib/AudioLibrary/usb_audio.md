# USB Audio nodes

`AudioInputUSB` receives host playback (PC to ESP32), while `AudioOutputUSB` sends the graph output to the host (ESP32 to PC), for example as a microphone/capture device.

## Initial backend

- Target: ESP32-S3 using the Arduino-ESP32 `USBAudioCard` API and TinyUSB.
- Initial format: signed interleaved 16-bit PCM at the current `AudioStream` sample rate; stereo by default.
- `input_usb.h` declares `AudioInputUSB`, which presents channel 0 as left and channel 1 as right.
- `output_usb.h` declares `AudioOutputUSB`, which accepts channel 0 as left and channel 1 as right.
- The public channel-count API is configurable up to `AUDIO_USB_MAX_CHANNELS` (default 8), but the initial `USBAudioCard` backend currently accepts only mono/stereo. Larger counts fail initialization instead of being silently truncated.
- Public files: `input_usb.h` and `output_usb.h`; shared transport implementation: `output_usb.cpp`; internal backend declarations: `usb_audio_backend.h`.
- Construct the USB nodes before `AudioMemory()`. USB descriptors are registered once, before the device is started.
- The library registers the audio interface before calling `USB.begin()`. Do not call `USB.begin()` before the audio nodes have registered their interfaces.

## FNK0104B / PlatformIO

Use the PioArduino stable PlatformIO platform for the FNK0104B environment. This supplies a recent Arduino-ESP32 3.x core with the `USBAudioCard` API (introduced in 3.3.8). Use USB-OTG/TinyUSB (`ARDUINO_USB_MODE=0`) and set `ARDUINO_USB_CDC_ON_BOOT=0`: auto-starting CDC calls `USB.begin()` before the runtime-created audio interface can register.

The backend registers the explicit `USBCDC USBSerial` instance and the UAC interface before calling `USB.begin()`. Use `USBSerial.print/println/printf()` for logging over the same USB cable. With CDC-on-boot disabled, Arduino's `Serial` maps to UART0 instead.

The environment defines `AUDIO_USB_REQUIRE_ARDUINO_UAC=1`, so the build fails clearly if the installed core does not expose enabled `USBAudioCard` / TinyUSB Audio support instead of silently enumerating CDC only.

**Unverified compatibility point:** Simultaneous CDC + UAC operation must still be confirmed on the FNK0104B with the selected Arduino-ESP32 core. This implementation has not yet passed a hardware test.

## Sample rate

The USB descriptor advertises one sample rate, matching `AudioStream::sampleRate()` when the device is initialized. Call `usbInput.setSampleRate(48000)` or `usbOutput.setSampleRate(48000)` before `AudioMemory()` if a 48 kHz graph is desired. Changing the rate after USB setup is rejected; host-driven runtime sample-rate changes are not implemented yet.
