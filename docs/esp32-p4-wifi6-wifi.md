# Waveshare ESP32-P4-WIFI6: Wi-Fi via ESP32-C6

The ESP32-P4 has no native radio. On the Waveshare ESP32-P4-WIFI6 board, an ESP32-C6 coprocessor provides Wi-Fi and Bluetooth through the ESP-Hosted SDIO link.

## PlatformIO environment

Use `[env:esp32-p4-wifi6]` in the root [platformio.ini](../platformio.ini).

- The environment uses `framework = arduino`, not the hybrid `arduino, espidf` framework.
- It is pinned to pioarduino `55.03.312-1` (Arduino-ESP32 3.3.12).
- The Arduino-ESP32 core provides the ESP-Hosted host support used to communicate with the onboard ESP32-C6.
- A separate ESP-IDF project build and project-level `sdkconfig.defaults` are not needed to build or run the P4 application with Wi-Fi.

The former `[env:esp32-p4-noWifi]` name is retained for compatibility with existing local build commands. The name is historical: it also uses the Arduino-only build and does not disable ESP-Hosted Wi-Fi when the C6 coprocessor is present.

The `extra_scripts` pre-build hook patches the ESP32-P4 SRAM linker-region expression in the installed framework package. This is a framework compatibility workaround, not a separate ESP-IDF application build.

## ESP-Hosted SDIO pins

The P4-side SDIO signals are:

| Signal | GPIO |
| --- | ---: |
| CLK | 18 |
| CMD | 19 |
| D0 | 14 |
| D1 | 15 |
| D2 | 16 |
| D3 | 17 |
| C6 reset | 54 |

These pins are reserved while Hosted Wi-Fi is enabled; do not reuse them for other peripherals. The onboard microSD interface uses different pins.

## Verify Wi-Fi connectivity

Copy [P4WifiStation.ino](../examples/HardwareTesting/P4WifiStation/P4WifiStation.ino) to your local PlatformIO project's `pio-test/`. Create an untracked `pio-test/secrets.h` locally with your network details:

```cpp
#define WIFI_SSID "your-2.4-GHz-SSID"
#define WIFI_PASS "your-password"
```

Do not commit real Wi-Fi credentials. The ESP32-C6 on this board uses 2.4 GHz Wi-Fi; a 5 GHz-only SSID will not work.

Build, upload and open the monitor:

```sh
pio run -e esp32-p4-wifi6
pio run -e esp32-p4-wifi6 -t upload
pio device monitor -e esp32-p4-wifi6
```

For an existing local setup that still uses the historical environment name, replace `esp32-p4-wifi6` with `esp32-p4-noWifi`.

The Internet Radio example uses the standard Arduino `WiFi` API and works with the same Arduino-only environment.

## Verified cold-start test

A physical-board test after completely removing power confirmed that the Arduino-only build initializes ESP-Hosted and reconnects the C6:

- Host and slave firmware both reported version `2.12.13`; the log reported `Versions Match!`.
- The station connected and received IP address `192.168.178.56`.
- The HTTPS radio stream connected, selected the MP3 decoder and started playback.
- Subsequent status messages continued to report Wi-Fi connected and MP3 playing.

This verifies that normal Wi-Fi/radio operation does not depend on a retained runtime state from a previous upload or reset.

## Optional C6 firmware maintenance

The normal P4 application build uses the C6 firmware already installed on the board. It does not build or flash a new C6 slave image.

[ P4WifiHostedOTA.ino ](../examples/HardwareTesting/P4WifiHostedOTA/P4WifiHostedOTA.ino) is an optional maintenance example for transferring a separately built C6 firmware image. The image must match the ESP-Hosted host version used by the Arduino core. The tested pair reported version `2.12.13`; do not flash an image built for a different host version without checking compatibility.

Only this optional operation may require a separate ESP-IDF installation/build for the ESP32-C6 slave project. That is independent of compiling and flashing the P4 application, and it is not required for normal Wi-Fi or radio use.
