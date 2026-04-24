#!/bin/bash
# Build script for Pico W Waxwing firmware
# Uses the Arm GNU Toolchain for bare-metal ARM

set -e

# Set the toolchain path - uses Arm-provided GCC 14.3 (compatible with Pico SDK 2.x)
export PATH="/Applications/ArmGNUToolchain/14.3.rel1/arm-none-eabi/bin:$PATH"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

# Clean out any stale CMake artifacts in the project root
rm -f "$SCRIPT_DIR"/CMakeCache.txt \
      "$SCRIPT_DIR"/cmake_install.cmake \
      "$SCRIPT_DIR"/pico_flash_region.ld

cd "$SCRIPT_DIR"

# Configure from the project root with explicit build dir to avoid stale cache conflicts
PICO_BOARD=pico_w cmake -B build

# Build
(cd build && make)

echo ""
echo "Build complete. Output files:"
ls -la "$SCRIPT_DIR"/build/*.uf2 "$SCRIPT_DIR"/build/*.bin "$SCRIPT_DIR"/build/*.elf 2>/dev/null
