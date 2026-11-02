#!/usr/bin/env python3
"""Run the private-path regression against the pinned SdFat implementation.

The fixture is an isolated in-memory FAT16 image. The real FatLib creates
/.crosspoint, the public controls and their long/short names; privacy_host.cpp
then reproduces the reviewed alias-open failure and checks the production
reader_sync::privateWebPath() from src/network/ReaderSyncStorage.cpp.

Prerequisites:
  * PlatformIO dependencies installed for the x4pro-gh_release environment
    (pio pkg install -e x4pro-gh_release), which provides .pio/libdeps/.../SdFat
    and ArduinoJson;
  * a C++20 host compiler (cc/c++ or $CXX);
  * no SD card, device, server or credentials are used.

The pinned FatLib/common sources are compiled unmodified. The host platform
shim supplies millis() and Arduino's __FlashStringHelper declaration, and the
unused SdCard compatibility header is built with SdFat's external SPI driver
selector (SPI_DRIVER_SELECT=3) so it does not need Arduino SPI.h.
"""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent

SDFAT = next(
    (p for p in (ROOT / ".pio/libdeps/x4pro-gh_release/SdFat", ROOT / ".pio/libdeps/x4pro/SdFat")
     if (p / "src/FatLib/FatVolume.cpp").is_file()),
    None,
)
if SDFAT is None:
    raise SystemExit("Install x4pro-gh_release PlatformIO dependencies first (pio pkg install -e x4pro-gh_release)")
DEPS = next(
    (p for p in (ROOT / ".pio/libdeps/x4pro-gh_release", ROOT / ".pio/libdeps/x4pro")
     if (p / "ArduinoJson/src/ArduinoJson.h").is_file()),
    None,
)
if DEPS is None:
    raise SystemExit("Install x4pro-gh_release PlatformIO dependencies first (ArduinoJson missing)")
ARDUINOJSON = DEPS / "ArduinoJson/src"

sources = [
    HERE / "fat/privacy_host.cpp",
    ROOT / "src/network/ReaderSyncStorage.cpp",
    *sorted((SDFAT / "src/FatLib").glob("*.cpp")),
    *sorted((SDFAT / "src/common").glob("*.cpp")),
]
flags = [
    "-std=c++20", "-O1", "-Wall", "-Wextra",
    # Host build of the same pinned sources: no Arduino runtime, same 8.3/LFN
    # behavior as the firmware (UTF-8 long names), RAM block device, RAII close.
    "-DENABLE_ARDUINO_FEATURES=0", "-DENABLE_ARDUINO_SERIAL=0", "-DENABLE_ARDUINO_STRING=0",
    "-DUSE_UTF8_LONG_NAMES=1", "-DUSE_BLOCK_DEVICE_INTERFACE=1", "-DDESTRUCTOR_CLOSES_FILE=1",
    # FatLib includes SdCard.h for historical reasons even in block-device
    # mode. SPI_DRIVER_SELECT=3 is SdFat's supported external-driver option: it
    # keeps the unused SdCard compatibility header free of Arduino's SPI.h
    # without touching FatLib, which is what actually implements FAT lookups.
    "-DSPI_DRIVER_SELECT=3",
    "-include", str(HERE / "fat/host_platform.h"),
    "-I", str(HERE / "fat"),
    "-I", str(ROOT / "src/network"),
    "-I", str(ROOT / "lib/Memory"),
    "-I", str(ARDUINOJSON),
    "-I", str(SDFAT / "src"),
]

with tempfile.TemporaryDirectory(prefix="reader-fat-privacy-") as scratch:
    binary = Path(scratch) / "privacy-host"
    command = [os.environ.get("CXX", "c++"), *flags, *map(str, sources), "-o", str(binary)]
    subprocess.run(command, check=True)
    subprocess.run([str(binary)], check=True)
    print("PASS: pinned SdFat private-path regression")
