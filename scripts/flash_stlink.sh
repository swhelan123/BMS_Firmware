#!/usr/bin/env bash
# flash_stlink.sh — flash BMS firmware to STM32F303VC via ST-Link.
#
# IMPORTANT: This script is DRY-RUN by default.
#   --execute is required to perform a real flash.
#   Read the output carefully before passing --execute.
#
# Usage:
#   ./scripts/flash_stlink.sh --app build_firmware/firmware.bin        # dry-run
#   ./scripts/flash_stlink.sh --app build_firmware/firmware.bin --execute
#   ./scripts/flash_stlink.sh --app build_firmware/firmware.hex         # hex also accepted
#
# Bootloader region (0x08000000):
#   --bootloader requires BOTH --bootloader and --execute.
#   Never flash the bootloader region without explicit intent.
#
# Options:
#   --app FILE          Firmware file (.bin or .hex or .elf)
#   --execute           Perform the actual flash (default: dry-run only)
#   --bootloader FILE   Flash to bootloader region (requires --execute as well)
#   --freq N            SWD frequency in kHz (default: 4000)
#   --programmer PATH   Override STM32_Programmer_CLI path
#   --help              Show this message
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# ── Flash map — must match bms_constants.h ────────────────────────────────────
APP_START_ADDR="0x08008000"
APP_MAX_SIZE=$((186 * 1024))   # 186 KB — last 2K page of app region = BL metadata
BL_START_ADDR="0x08000000"
BL_SIZE=$((32 * 1024))         # 32 KB
CONFIG_A_ADDR="0x08037000"

# ── Args ──────────────────────────────────────────────────────────────────────

APP_FILE=""
BL_FILE=""
EXECUTE=0
SWD_FREQ=4000
PROGRAMMER="${STM32_PROGRAMMER:-}"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --app)          APP_FILE="$2"; shift 2 ;;
        --bootloader)   BL_FILE="$2"; shift 2 ;;
        --execute)      EXECUTE=1; shift ;;
        --freq)         SWD_FREQ="$2"; shift 2 ;;
        --programmer)   PROGRAMMER="$2"; shift 2 ;;
        --help|-h)
            sed -n '/^# Usage/,/^[^#]/p' "$0" | grep '^#' | sed 's/^# \?//'
            exit 0 ;;
        *)  echo "Unknown option: $1" >&2; exit 1 ;;
    esac
done

# ── Validate arguments ────────────────────────────────────────────────────────

if [[ -z "$APP_FILE" && -z "$BL_FILE" ]]; then
    echo "error: specify --app FILE or --bootloader FILE" >&2
    echo "       run with --help for usage" >&2
    exit 1
fi

if [[ -n "$BL_FILE" && "$EXECUTE" -eq 0 ]]; then
    echo "error: --bootloader requires --execute (explicit acknowledgement required)" >&2
    echo "       Bootloader flashing is irreversible without a working app flash." >&2
    exit 1
fi

# ── Locate programmer ─────────────────────────────────────────────────────────

if [[ -z "$PROGRAMMER" ]]; then
    if command -v STM32_Programmer_CLI &>/dev/null; then
        PROGRAMMER="$(command -v STM32_Programmer_CLI)"
    else
        PROGRAMMER="STM32_Programmer_CLI"   # will fail at execution time if absent
    fi
fi

# ── Helper: validate a firmware file ─────────────────────────────────────────

_check_file() {
    local file="$1"
    local max_bytes="$2"
    local label="$3"

    if [[ ! -f "$file" ]]; then
        echo "error: file not found: $file" >&2
        echo "       Run ./scripts/build_firmware.sh first." >&2
        exit 1
    fi

    # Size check only for .bin files (hex/elf are not raw images)
    if [[ "$file" == *.bin ]]; then
        local size
        size="$(wc -c < "$file" | tr -d ' ')"
        if [[ "$size" -eq 0 ]]; then
            echo "error: $file is empty" >&2
            exit 1
        fi
        if [[ "$size" -gt "$max_bytes" ]]; then
            echo "error: $file is ${size} bytes — exceeds ${label} region (${max_bytes} bytes)" >&2
            echo "       Flash region: ${label}" >&2
            echo "       Image size:   ${size} bytes" >&2
            echo "       Max allowed:  ${max_bytes} bytes" >&2
            exit 1
        fi
        echo "    file:    $file"
        echo "    size:    ${size} bytes  (max ${max_bytes})"
    else
        echo "    file:    $file"
        echo "    format:  ${file##*.}  (no size check; hex/elf address-embedded)"
    fi
}

# ── Helper: build and print flash command ─────────────────────────────────────

_flash_command() {
    local file="$1"
    local addr="$2"
    echo "$PROGRAMMER" \
        -c "port=SWD" "freq=${SWD_FREQ}" "reset=HWrst" \
        -d "$file" \
        -s "$addr" \
        -v
}

# ── App flash ────────────────────────────────────────────────────────────────

if [[ -n "$APP_FILE" ]]; then
    echo
    echo "==> Application flash"
    echo "    target:  STM32F303VC via SWD"
    echo "    address: ${APP_START_ADDR}  (app region; bootloader at ${BL_START_ADDR} untouched)"
    _check_file "$APP_FILE" "$APP_MAX_SIZE" "application"
    echo "    programmer: $PROGRAMMER"

    CMD="$(_flash_command "$APP_FILE" "$APP_START_ADDR")"
    echo
    echo "    Command:"
    echo "      $CMD"
    echo

    if [[ "$EXECUTE" -eq 0 ]]; then
        echo "    *** DRY-RUN ONLY — pass --execute to perform the actual flash ***"
        echo
    else
        echo "    ==> Executing flash..."
        echo
        $CMD
        echo
        echo "    ==> App flash complete."
    fi
fi

# ── Bootloader flash ─────────────────────────────────────────────────────────

if [[ -n "$BL_FILE" ]]; then
    echo
    echo "==> Bootloader flash  *** CAUTION: writes to ${BL_START_ADDR} ***"
    echo "    target:  STM32F303VC via SWD"
    echo "    address: ${BL_START_ADDR}  (bootloader region)"
    _check_file "$BL_FILE" "$BL_SIZE" "bootloader"
    echo "    programmer: $PROGRAMMER"

    CMD="$(_flash_command "$BL_FILE" "$BL_START_ADDR")"
    echo
    echo "    Command:"
    echo "      $CMD"
    echo
    echo "    ==> Executing bootloader flash (--execute was passed)..."
    echo
    $CMD
    echo
    echo "    ==> Bootloader flash complete."
fi
