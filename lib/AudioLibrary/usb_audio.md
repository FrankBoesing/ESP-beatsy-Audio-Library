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

Use the PioArduino platform release `55.03.311`, which supplies Arduino-ESP32 3.3.11 and includes the `USBAudioCard` API introduced in 3.3.8. The project pins this release instead of the moving `stable` URL so an older cached framework package is less likely to be reused. Use USB-OTG/TinyUSB (`ARDUINO_USB_MODE=0`) and set `ARDUINO_USB_CDC_ON_BOOT=0`: auto-starting CDC calls `USB.begin()` before the runtime-created audio interface can register.

The backend registers the explicit `USBCDC USBSerial` instance and the UAC interface before calling `USB.begin()`. Use `USBSerial.print/println/printf()` for logging over the same USB cable. With CDC-on-boot disabled, Arduino's `Serial` maps to UART0 instead.

The environment defines `AUDIO_USB_REQUIRE_ARDUINO_UAC=1`, so the build fails clearly if the installed core does not expose enabled `USBAudioCard` / TinyUSB Audio support instead of silently enumerating CDC only.

**Unverified compatibility point:** Simultaneous CDC + UAC operation must still be confirmed on the FNK0104B with the selected Arduino-ESP32 core. This implementation has not yet passed a hardware test.

## Sample rate

The USB descriptor advertises one sample rate, matching `AudioStream::sampleRate()` when the device is initialized. Call `usbInput.setSampleRate(48000)` or `usbOutput.setSampleRate(48000)` before `AudioMemory()` if a 48 kHz graph is desired. Changing the rate after USB setup is rejected; host-driven runtime sample-rate changes are not implemented yet.


## Troubleshooting log — 2026-10-11

This section records the current hardware/software state so the next debugging session can resume without repeating the investigation.

### Current target configuration

- Board: Freenove FNK0104B / ESP32-S3, enumerating as the generic ESP32-S3-DevKitC-1 USB identity.
- PlatformIO environment: `freenove_esp32_s3_display`.
- Platform is pinned to PioArduino release `55.03.311`, providing Arduino-ESP32 3.3.11 and `USBAudioCard.h`.
- USB mode: native USB-OTG/TinyUSB (`ARDUINO_USB_MODE=0`).
- `ARDUINO_USB_CDC_ON_BOOT=0`: this is intentional. With CDC auto-start enabled, Arduino starts TinyUSB before the runtime-created `USBAudioCard` registers its descriptor, resulting in CDC-only enumeration.
- The backend creates `USBCDC USBSerial`, registers the audio interface, calls `USBSerial.begin()`, and then calls `USB.begin()`. When logging over the same USB connection, use `USBSerial.print/println/printf()`; with CDC-on-boot disabled, `Serial` maps to UART0.

### Enumeration history

1. **Before changing CDC startup:** USBTreeView showed a configuration descriptor of 75 bytes with two interfaces, both belonging to CDC. COM13 appeared, but no audio interfaces were present. This is consistent with TinyUSB having already started before the audio interface was registered.
2. **After disabling CDC-on-boot and registering CDC + audio before `USB.begin()`:** the configuration descriptor reports `wTotalLength = 0x00FD` (253 bytes) and `bNumInterfaces = 5`. COM13 is present, and Windows enumerates three child devices named `TinyUSB UAC1 (USB-Audiogerät)`, corresponding to the Audio Control interface and the two streaming interfaces.
3. Unplugging and reconnecting the USB cable changed the parent device status from `DeviceGeneralFailure` back to `DeviceConnected`, but did **not** fix audio startup. COM13 remains enumerated. Whether log text is reaching COM13 must be verified separately with `USBSerial`.

### Current failure

All three Windows media child devices report:

- `Problem: 10 (CM_PROB_FAILED_START)`
- Driver service: `usbaudio`
- USB parent VID/PID: `303A:1001`
- Device operates at Full-Speed.

Therefore, USB enumeration now includes audio, but Windows still cannot start the UAC1 interfaces. There is no confirmed audio playback or capture yet.

### Relevant descriptor values from USBTreeView

- Configuration: 5 interfaces, 253 bytes total.
- Audio Control interface: interface 2; class-specific AC header length field reports 65 bytes and references streaming interfaces 3 and 4.
- Streaming interface 3 (host playback / OUT): alternate setting 0 has no endpoints; alternate setting 1 has one isochronous OUT endpoint, address `0x01`, `bmAttributes = 0x09` (adaptive), `wMaxPacketSize = 0x00C4` (196 bytes), `bInterval = 1`.
- Streaming interface 4 (device capture / IN): alternate setting 0 has no endpoints; alternate setting 1 has one isochronous IN endpoint, address `0x81`, `bmAttributes = 0x05` (asynchronous), `wMaxPacketSize = 0x00C4` (196 bytes), `bInterval = 1`.
- Both streams advertise stereo, 16-bit PCM at 48 kHz.
- The descriptor totals and interface numbering appear internally consistent from the USBTreeView dump; this does **not** prove that Windows accepts every class-specific detail.

### Assessment of proposed descriptor fixes

Do not change the endpoint size from 196 to 192 bytes just because the average stereo payload at 48 kHz / 16-bit is 192 bytes per millisecond. TinyUSB's `TUD_AUDIO_EP_SIZE(false, 48000, 2, 2)` formula deliberately reserves a maximum packet size of 196 bytes to allow for packet/frame variation. The observed adaptive OUT and asynchronous IN attributes also match Espressif's UAC1 headset descriptor template.

References for these implementation details:

- [Espressif Arduino-ESP32 3.3.11 UAC descriptors](https://github.com/espressif/arduino-esp32/blob/3.3.11/libraries/USB/src/USBAudioCardDescriptors.h)
- [TinyUSB endpoint-size formula](https://github.com/hathach/tinyusb/blob/efab607b8b738224c27b5e9e6410f8c74da43373/src/device/usbd.h)

Gemini's hypothesis that strict Windows parsing or endpoint handling may be involved remains possible, but the packet size and synchronization attributes are not, by themselves, demonstrated defects. Switching this implementation to UAC2 is not a one-line flag change: the current Espressif `USBAudioCard` implementation builds UAC1 descriptors in Full-Speed mode; its UAC2 descriptor path is selected for a High-Speed build.

### Windows UAC1 issue — possible confounding factor

Microsoft's release-health documentation describes a Windows issue introduced by the September 8, 2026 security update where some USB Audio Class 1.0 devices fail to start or produce sound, including Device Manager Code 10. The September 14 out-of-band update addressed a subset of symptoms involving multichannel modes (including 8-channel / 3D configurations), but Microsoft's documentation still lists unresolved UAC1 startup/no-audio symptoms as of September 29, 2026, affecting Windows 11 25H2 and 26H2 among other versions.

- [Microsoft: KB5129195 (September 14, 2026)](https://support.microsoft.com/en-us/servicing/os/windows-11/2026/09/kb5129195-windows-11-24h2-25h2-security-update)
- [Microsoft: Windows 11 25H2 known issues](https://learn.microsoft.com/en-us/windows/release-health/status-windows-11-25H2)
- [Microsoft: Windows 11 26H2 known issues](https://learn.microsoft.com/en-us/windows/release-health/status-windows-11-26H2)

The user's reported Windows build is `26200.9457` (Windows 11 25H2). This is a plausible confounding factor because the reported symptom closely matches Microsoft's UAC1 Code 10 issue, but it is **not proven to be the cause** of this device's failure.

### Next diagnostic step

Before changing descriptors or attempting UAC2, test the **same firmware and board** on another host, ideally Linux or another up-to-date Windows PC.

- If audio starts on the second host, investigate Windows-specific compatibility/updates first.
- If it also fails on another host, capture the `USBSerial` startup output and continue with TinyUSB/descriptor validation.
- Keep the current failing build and USBTreeView dump as the baseline; do not change `wMaxPacketSize`, synchronization attributes, or terminal links by guesswork.

**Status as of 2026-10-11:** USB CDC and UAC1 interfaces enumerate; UAC1 startup remains unresolved; no confirmed successful audio round-trip or cross-host test yet.
