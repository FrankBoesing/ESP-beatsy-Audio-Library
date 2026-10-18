Import("env")

from pathlib import Path

# Temporary diagnostic patch for Arduino-ESP32 3.3.12:
# Windows sees the CDC function IAD, so the UAC1 AudioControl + AudioStreaming
# interfaces must also be grouped explicitly as one function. This adds an
# Audio IAD immediately before the microphone's AudioControl interface.
framework_dir = env.PioPlatform().get_package_dir("framework-arduinoespressif32")
if not framework_dir:
    raise RuntimeError("Could not locate framework-arduinoespressif32 package")

source = Path(framework_dir) / "libraries" / "USB" / "src" / "USBAudioCard.cpp"
if not source.is_file():
    raise RuntimeError("USBAudioCard.cpp not found: " + str(source))

project_dir = Path(env.subst("$PROJECT_DIR"))
backup = project_dir / ".pio" / "framework-backup" / "USBAudioCard.cpp.original"
backup.parent.mkdir(parents=True, exist_ok=True)

original = source.read_text(encoding="utf-8")
marker = "ESP_BEATSY_UAC1_AUDIO_IAD"
if marker in original:
    print("USB AUDIO IAD diagnostic patch already present.")
else:
    descriptor_old = '''    uint8_t descriptor[TUD_AUDIO10_MICROPHONE_DESC_LEN(1)] = {
      // Interface number, string index, EP In address, max sample rate, microphone channels, bytes per sample RX/TX, bits used per sample RX/TX, sample rate
      TUD_AUDIO10_MICROPHONE_DESCRIPTOR(_itf_num, str_index, (uint8_t)(ep_num | 0x80), 48000, _mic_channels, _bytes_per_sample, _bits_per_sample, _sample_rate)
    };
    *itf += 2;
    memcpy(dst, descriptor, TUD_AUDIO10_MICROPHONE_DESC_LEN(1));
    return TUD_AUDIO10_MICROPHONE_DESC_LEN(1);'''

    descriptor_new = '''    uint8_t descriptor[TUD_AUDIO10_MICROPHONE_DESC_LEN(1) + 8] = {
      // ESP_BEATSY_UAC1_AUDIO_IAD: group AudioControl + AudioStreaming for composite Windows enumeration.
      8, 0x0B, _itf_num, 2, 0x01, 0x01, 0x00, 0x00,
      // Interface number, string index, EP In address, max sample rate, microphone channels, bytes per sample RX/TX, bits used per sample RX/TX, sample rate
      TUD_AUDIO10_MICROPHONE_DESCRIPTOR(_itf_num, str_index, (uint8_t)(ep_num | 0x80), 48000, _mic_channels, _bytes_per_sample, _bits_per_sample, _sample_rate)
    };
    *itf += 2;
    memcpy(dst, descriptor, TUD_AUDIO10_MICROPHONE_DESC_LEN(1) + 8);
    return TUD_AUDIO10_MICROPHONE_DESC_LEN(1) + 8;'''

    descriptor_len_old = "      descriptor_len = TUD_AUDIO10_MICROPHONE_DESC_LEN(1);"
    descriptor_len_new = "      descriptor_len = TUD_AUDIO10_MICROPHONE_DESC_LEN(1) + 8; // ESP_BEATSY_UAC1_AUDIO_IAD"

    if original.count(descriptor_old) != 1:
        raise RuntimeError("Could not uniquely identify the UAC1 microphone descriptor block; framework version may have changed.")
    if original.count(descriptor_len_old) != 1:
        raise RuntimeError("Could not uniquely identify the UAC1 microphone descriptor length; framework version may have changed.")

    if not backup.exists():
        backup.write_text(original, encoding="utf-8")
        print("Saved original Arduino USB framework source to: " + str(backup))
    patched = original.replace(descriptor_old, descriptor_new, 1)
    patched = patched.replace(descriptor_len_old, descriptor_len_new, 1)
    source.write_text(patched, encoding="utf-8")
    print("Patched USBAudioCard.cpp: added UAC1 Audio IAD (8 bytes).")
    print("Patched framework source: " + str(source))
