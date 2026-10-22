# Waveshare ESP32-P4-WIFI6: Wi-Fi via ESP32-C6

The ESP32-P4 has no native radio. On this board, an ESP32-C6 coprocessor provides Wi-Fi and Bluetooth through the ESP-Hosted SDIO link.

## PlatformIO environment

Use [env:esp32-p4-wifi6] in the root [platformio.ini](../platformio.ini). This environment differs from the audio-only P4-Pico profile:

- It uses \`framework = arduino, espidf\`, so the Arduino core is built as an ESP-IDF component.
- It is pinned to pioarduino \`55.03.312-1\` (Arduino-ESP32 3.3.12 / ESP-IDF 5.5.5) as a compatibility baseline for the Hosted components.
- The P4 host uses ESP-Hosted and \`esp_wifi_remote\` from the Arduino core's ESP-IDF component manifest.
- \`board_build.cmake_extra_args\` selects the tracked \`sdkconfig.defaults.esp32-p4-wifi6\` through \`SDKCONFIG_DEFAULTS\`. This is needed because pioarduino ignores \`custom_sdkconfig\` in the combined \`arduino, espidf\` mode. The defaults select the C6 coprocessor, a 1 kHz FreeRTOS tick rate, the mbedTLS PSK Kconfig switches required to compile Arduino-ESP32 3.3.12's \`ssl_client.cpp\`, and the P4-side SDIO pin preset.

The P4-side SDIO signals are CLK=GPIO18, CMD=GPIO19, D0-D3=GPIO14-17, with C6 reset on GPIO54. These pins are reserved while Hosted Wi-Fi is enabled; do not reuse them for other peripherals. The onboard microSD interface uses different pins.

The P4-WIFI6 profile also enables the 32 MB stacked PSRAM in HEX mode at 200 MHz via `CONFIG_SPIRAM`, `CONFIG_SPIRAM_MODE_HEX`, and `CONFIG_SPIRAM_SPEED_200M`. The `extra_scripts` pre-build hook patches the unparenthesized `SRAM_START`/`SRAM_SIZE` macro expressions in the pinned ESP-IDF 5.5.5 ESP32-P4 linker template; this fixes an oversized `sram_seg` length that otherwise triggers `cannot assign memory region sram_seg to chip memory type`. The hook validates the expected upstream text and fails the build if the framework template changes unexpectedly.

The P4 bootloader image overlaps the default partition-table address `0x8000` in this configuration. The SDK defaults therefore set `CONFIG_PARTITION_TABLE_OFFSET=0x10000`, and `partitions_esp32_p4_wifi6_32MB.csv` explicitly places NVS at `0x11000`, OTA data at `0x16000`, and the `ota_0` application partition at `0x20000`. The application uses the `ota_0` subtype so both the ESP-IDF and PlatformIO hybrid build logic derive the actual application offset from the partition table. The app partition ends at `0x1FF0000`, immediately before the 64 KiB coredump partition. The configured PlatformIO maximum app size is kept equal to that app partition size (`0x1FD0000` = 33,357,824 bytes). This partition-table offset change must be applied consistently to the bootloader, partition table and application; after changing SDK defaults, rename the generated `sdkconfig.esp32-p4-wifi6` before rebuilding.

## Verify Wi-Fi connectivity

Copy [P4WifiStation.cpp](../examples/HardwareTesting/P4WifiStation/P4WifiStation.cpp) to your local PlatformIO project's \`src/main.cpp\`. Create \`src/secrets.h\` locally with your network details:

\`\`\`cpp
#define WIFI_SSID "your-2.4-GHz-SSID"
#define WIFI_PASS "your-password"
\`\`\`

Do not commit real Wi-Fi credentials. The ESP32-C6 on this board uses 2.4 GHz Wi-Fi; a 5 GHz-only SSID will not work.

If changing the SDK defaults after the first build, rename or remove the generated \`sdkconfig.esp32-p4-wifi6\` first; otherwise ESP-IDF will reuse its previous values. Keep a backup if you have customized it.

Build, upload and open the monitor:

\`\`\`sh
pio run -e esp32-p4-wifi6 -t clean
pio run -e esp32-p4-wifi6
pio run -e esp32-p4-wifi6 -t upload
pio device monitor -e esp32-p4-wifi6
\`\`\`

The example reports the connection status, DHCP address and RSSI. The existing Beatsy Internet Radio example uses the standard Arduino \`WiFi\` API, so it can use the same environment after credentials are configured.

## Build the matching ESP-Hosted C6 firmware

Arduino-ESP32 3.3.12 reports Hosted host version **2.13.0** and generates the URL \`https://espressif.github.io/arduino-esp32/hosted/esp32c6-v2.13.0.bin\`. That file is not present in the published \`gh-pages/hosted\` directory. The built-in \`ESP_HostedOTA\` updater therefore receives HTTP 404. Do not substitute an older binary without confirming host/slave compatibility.

Build the slave image from the **official ESP-Hosted v2.13.0 component** using ESP-IDF 5.5.5:

1. Open the ESP-IDF 5.5.5 PowerShell command prompt. Use a clean work directory outside the PlatformIO project.
2. Create the version-pinned slave example:

   \`\`\`powershell
   idf.py create-project-from-example "espressif/esp_hosted==2.13.0:slave"
   cd slave
   idf.py set-target esp32c6
   idf.py build
   \`\`\`

3. The project is named \`network_adapter\`; the expected application image is \`build/network_adapter.bin\`. The v2.13.0 source generates the image version from its component manifest, so do not edit the generated version header.
4. The slave project's defaults select SDIO on supported chips, enable Wi-Fi, and use a 4 MB flash configuration with two OTA slots. Keep the development-board choice at \`ESP_HOST_DEV_BOARD_NONE\` unless you verify that the Waveshare board needs a different **C6-side** board mapping. The P4-side pin assignment (18/19/14–17/54) is separate from the C6's SDIO pins.

Official sources: [ESP-Hosted v2.13.0 slave example](https://github.com/espressif/esp-hosted-mcu/tree/v2.13.0/slave), [slave Kconfig](https://github.com/espressif/esp-hosted-mcu/blob/v2.13.0/slave/main/Kconfig.projbuild), [component v2.13.0](https://components.espressif.com/components/espressif/esp_hosted/versions/2.13.0/readme).

## Serve the built image locally

This avoids relying on the missing Espressif URL and avoids putting a firmware image in the library repository.

1. Copy \`build/network_adapter.bin\` to a dedicated directory on your PC, for example \`C:\\hosted-fw\\network_adapter.bin\`.
2. Open PowerShell in that directory and start a temporary server on your trusted home LAN:

   \`\`\`powershell
   python -m http.server 8000 --bind 0.0.0.0
   \`\`\`

3. Run \`ipconfig\` and note the PC's IPv4 address for the same WLAN/Ethernet LAN as the P4. If the PC address is \`192.168.178.10\`, the image URL will be \`http://192.168.178.10:8000/network_adapter.bin\`. Allow Python through Windows Firewall for private networks if prompted. Do not expose this server to the public internet.
4. In your local, untracked \`src/secrets.h\`, keep the Wi-Fi definitions and add the image URL:

   \`\`\`cpp
   #define WIFI_SSID "your-2.4-GHz-SSID"
   #define WIFI_PASS "your-password"
   #define HOSTED_FW_URL "http://192.168.178.10:8000/network_adapter.bin"
   \`\`\`

   Replace the example IP with your PC's actual LAN IP. From another device on the same LAN, the URL should return the binary file, not a directory listing or an HTML error page.

## Run the custom OTA updater

Copy [P4WifiHostedOTA.cpp](../examples/HardwareTesting/P4WifiHostedOTA/P4WifiHostedOTA.cpp) to your local PlatformIO project's \`src/main.cpp\`. It downloads the explicit \`HOSTED_FW_URL\`, streams the image to the C6 through the core's \`hostedBeginUpdate/hostedWriteUpdate/hostedEndUpdate/hostedActivateUpdate\` calls, and restarts the P4 after success. It does not use the hard-coded, currently missing v2.13.0 URL from \`ESP_HostedOTA\`.

\`\`\`powershell
pio run -e esp32-p4-wifi6 -t clean
pio run -e esp32-p4-wifi6
pio run -e esp32-p4-wifi6 -t upload
pio device monitor -e esp32-p4-wifi6
\`\`\`

Keep the local Python HTTP server running and do not power off the board during the update. On successful reboot, the host should reconnect and report C6 firmware version 2.13.0. If the version query still times out, stop and collect the complete serial log before retrying.

The custom updater keeps the HTTP client and network client lifetimes correctly ordered, checks for HTTP/stream failures and has an idle timeout. If the transfer itself fails after OTA has started, treat the C6's state as unconfirmed until the next reboot and log review.

The Wi-Fi station test has been verified on the physical board based on the reported serial log. The custom C6 image build and OTA flow still need to be validated on the physical board.
