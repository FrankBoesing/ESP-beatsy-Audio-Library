#!/usr/bin/env python3
"""Restore the saved, unmodified USBAudioCard.cpp framework source."""

import os
from pathlib import Path
import shutil
import sys

project_dir = Path(__file__).resolve().parents[1]
backup = project_dir / ".pio" / "framework-backup" / "USBAudioCard.cpp.original"
if not backup.is_file():
    print("No framework backup found at:", backup)
    sys.exit(1)

core_dir = Path(os.environ.get("PLATFORMIO_CORE_DIR", Path.home() / ".platformio"))
source = core_dir / "packages" / "framework-arduinoespressif32" / "libraries" / "USB" / "src" / "USBAudioCard.cpp"
if not source.is_file():
    print("Framework source not found at:", source)
    print("Set PLATFORMIO_CORE_DIR to the PlatformIO Core directory, then retry.")
    sys.exit(1)

current = source.read_text(encoding="utf-8")
if "ESP_BEATSY_UAC1_AUDIO_IAD" not in current:
    print("Framework source does not contain the diagnostic patch; nothing to restore.")
    sys.exit(0)

shutil.copy2(backup, source)
print("Restored original framework source:", source)
