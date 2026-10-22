# Waveshare ESP32-P4-WIFI6: Wi-Fi via ESP32-C6

The ESP32-P4 has no native radio. On this board, an ESP32-C6 coprocessor provides Wi-Fi and Bluetooth through the ESP-Hosted SDIO link.

## PlatformIO environment

Use `[env:esp32-p4-wifi6]` in the root `platformio.ini`. This environment differs from the audio-only P4-Pico profile:

- It uses `framework = arduino, espidf`, so the Arduino core is built as an ESP-IDF component.
- It is pinned to pioarduino `55.03.312-1` (Arduino-ESP32 3.3.12 / ESP-IDF 5.5.5) as a compatibility baseline for the hosted components.
- The ESP32-P4 target pulls `espressif/esp_hosted` and `espressif/esp_wifi_remote` from the Arduino core's ESP-IDF component manifest.
- `custom_sdkconfig` selects the ESP32-C6 coprocessor and the P4 Function-EV-compatible SDIO pin preset used by the Waveshare board.

The SDIO signals are CLK=GPIO18, CMD=GPIO19, D0-D3=GPIO14-17, with C6 reset on GPIO54. These pins are reserved while Hosted Wi-Fi is enabled; do not reuse them for other peripherals. The onboard microSD interface uses different pins.

The first build needs access to the ESP Component Registry so the required components can be resolved.

## Verify Wi-Fi connectivity

Copy `examples/HardwareTesting/P4WifiStation/P4WifiStation.cpp` to your local PlatformIO project's `src/main.cpp`. Create `src/secrets.h` locally with your network details:

```cpp
#define WIFI_SSID "your-2.4-GHz-SSID"
#define WIFI_PASS "your-password"
```

Do not commit real Wi-Fi credentials. The ESP32-C6 on this board uses 2.4 GHz Wi-Fi; a 5 GHz-only SSID will not work.

Build, upload and open the monitor:

```sh
pio run -e esp32-p4-wifi6 -t clean
pio run -e esp32-p4-wifi6
pio run -e esp32-p4-wifi6 -t upload
pio device monitor -e esp32-p4-wifi6
```

The example reports the connection status, DHCP address and RSSI. The existing Beatsy Internet Radio example uses the standard Arduino `WiFi` API, so it can use the same environment after credentials are configured.

## C6 coprocessor firmware

Successful compilation does not prove that Wi-Fi works at runtime. The ESP32-C6 must be running ESP-Hosted slave firmware compatible with the host-side `esp_hosted` / `esp_wifi_remote` components resolved by the build. This repository does not include a C6 firmware image. If startup logs report a Hosted initialization or protocol-version error, check the installed C6 firmware and follow the official Waveshare procedure to flash a compatible slave image through the C6 UART pads. Do not flash the P4 application image to the C6.

This environment has not yet been verified by compiling and connecting on the physical board; hardware validation remains necessary.
