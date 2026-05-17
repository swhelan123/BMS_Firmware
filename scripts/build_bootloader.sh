#!/usr/bin/env bash
# build_bootloader.sh — build BMS bootloader for STM32F303VC.
#
# Dependencies:
#   arm-none-eabi-gcc (GNU Arm Embedded Toolchain)
#   cmake >= 3.20
#   ninja
#
# Usage:
#   ./scripts/build_bootloader.sh [debug|release]
#
# WARNING: The bootloader has NOT been flashed or validated on hardware.
#          Do not flash to a live system without completing the first-flash
#          readiness checks in docs/first_flash_guide.md.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_TYPE="${1:-release}"
BUILD_DIR="${REPO_ROOT}/build_bootloader"

case "${BUILD_TYPE}" in
  release|Release)
    CMAKE_BUILD_TYPE="Release"
    ;;
  debug|Debug)
    CMAKE_BUILD_TYPE="Debug"
    ;;
  *)
    echo "error: build type must be 'debug' or 'release'" >&2
    exit 1
    ;;
esac

echo "==> BMS Bootloader Build (${CMAKE_BUILD_TYPE}) [NOT HARDWARE-VALIDATED]"
echo "    Toolchain: $(arm-none-eabi-gcc --version | head -1)"

cmake -B "${BUILD_DIR}" \
      -DCMAKE_TOOLCHAIN_FILE="${REPO_ROOT}/firmware/cmake/arm_none_eabi.cmake" \
      -DCMAKE_BUILD_TYPE="${CMAKE_BUILD_TYPE}" \
      -G Ninja \
      -S "${REPO_ROOT}/bootloader"

cmake --build "${BUILD_DIR}" --target bms_bootloader.elf

echo ""
echo "==> Build artifacts:"
ls -lh "${BUILD_DIR}/bootloader.bin" "${BUILD_DIR}/bootloader.hex" \
        "${BUILD_DIR}/bms_bootloader.elf" 2>/dev/null || true
echo ""
echo "WARNING: Bootloader is software-complete but not hardware-validated."
echo "         Do not flash without completing docs/first_flash_guide.md."
