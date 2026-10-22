Import("env")

from pathlib import Path
import re

# ESP-IDF 5.5.5's ESP32-P4 linker template leaves SRAM_START and SRAM_SIZE
# unparenthesized. Since macros are textually expanded, SRAM_SIZE becomes:
#   SRAM_END - 0x4FF00000 + CONFIG_CACHE_L2_CACHE_SIZE
# instead of subtracting the cache size. That produces an oversized sram_seg
# and the "cannot assign memory region sram_seg to chip memory type" warning.
# Patch only this known template form; fail loudly if the upstream file changes.

framework_dir = env.PioPlatform().get_package_dir("framework-espidf")
if not framework_dir:
    raise RuntimeError("Could not locate framework-espidf package")

source = Path(framework_dir) / "components" / "esp_system" / "ld" / "esp32p4" / "memory.ld.in"
if not source.is_file():
    raise RuntimeError("ESP32-P4 linker template not found: " + str(source))

original = source.read_text(encoding="utf-8")
marker = "ESP_BEATSY_P4_SRAM_SIZE_FIX"

if marker in original:
    print("ESP32-P4 SRAM linker-size fix already present.")
else:
    start_pattern = r"(?m)^#define\\s+SRAM_START\\s+0x4FF00000\\s+\\+\\s+CONFIG_CACHE_L2_CACHE_SIZE\\s*$"
    size_pattern = r"(?m)^#define\\s+SRAM_SIZE\\s+SRAM_END\\s*-\\s*SRAM_START\\s*$"

    if len(re.findall(start_pattern, original)) != 1:
        raise RuntimeError("Unexpected SRAM_START definition in ESP32-P4 linker template; refusing to patch.")
    if len(re.findall(size_pattern, original)) != 1:
        raise RuntimeError("Unexpected SRAM_SIZE definition in ESP32-P4 linker template; refusing to patch.")

    project_dir = Path(env.subst("$PROJECT_DIR"))
    backup = project_dir / ".pio" / "framework-backup" / "esp32p4-memory.ld.in.original"
    backup.parent.mkdir(parents=True, exist_ok=True)
    if not backup.exists():
        backup.write_text(original, encoding="utf-8")
        print("Saved original ESP32-P4 linker template to: " + str(backup))

    patched = re.sub(
        start_pattern,
        "#define SRAM_START       (0x4FF00000 + CONFIG_CACHE_L2_CACHE_SIZE) /* " + marker + " */",
        original,
        count=1,
    )
    patched = re.sub(
        size_pattern,
        "#define SRAM_SIZE        (SRAM_END - SRAM_START)",
        patched,
        count=1,
    )
    source.write_text(patched, encoding="utf-8")
    print("Patched ESP32-P4 SRAM linker-region arithmetic.")
