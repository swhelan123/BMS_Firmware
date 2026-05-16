#!/usr/bin/env bash
# validate_all.sh — one-command validation of the full BMS repo.
#
# Usage:
#   ./scripts/validate_all.sh
#   ./scripts/validate_all.sh --no-firmware   # skip STM32 build (no toolchain needed)
#
# Exit codes:
#   0  all sections passed
#   1  one or more sections failed
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_ROOT"

SKIP_FIRMWARE=0
for arg in "$@"; do
    [[ "$arg" == "--no-firmware" ]] && SKIP_FIRMWARE=1
done

PASS=0
FAIL=0
SKIPPED=0

# ── Helpers ───────────────────────────────────────────────────────────────────

_header() { echo; echo "══════════════════════════════════════════"; echo "  $1"; echo "══════════════════════════════════════════"; }
_ok()     { echo "  ✓  $1"; PASS=$((PASS+1)); }
_fail()   { echo "  ✗  $1"; FAIL=$((FAIL+1)); }
_skip()   { echo "  ─  $1 (skipped)"; SKIPPED=$((SKIPPED+1)); }

_run() {
    local label="$1"; shift
    if "$@" > /tmp/validate_out.txt 2>&1; then
        _ok "$label"
    else
        _fail "$label"
        echo "    --- output ---"
        sed 's/^/    /' /tmp/validate_out.txt | head -30
        echo "    ---"
    fi
}

# ── Tool detection ─────────────────────────────────────────────────────────────

_header "Tool detection"

PYTHON=""
for py in python3 python; do
    if command -v "$py" &>/dev/null && "$py" -c "import sys; assert sys.version_info>=(3,11)" 2>/dev/null; then
        PYTHON="$py"
        break
    fi
done
if [[ -n "$PYTHON" ]]; then
    _ok "python3.11+ found: $("$PYTHON" --version)"
else
    _fail "python3.11+ not found — install Python 3.11+"
fi

HAS_PYSERIAL=0
if [[ -n "$PYTHON" ]] && "$PYTHON" -c "import serial" 2>/dev/null; then
    _ok "pyserial installed"
    HAS_PYSERIAL=1
else
    _skip "pyserial not installed (serial port tests will skip)"
fi

HAS_PYQT6=0
if [[ -n "$PYTHON" ]] && "$PYTHON" -c "import PyQt6" 2>/dev/null; then
    _ok "PyQt6 installed"
    HAS_PYQT6=1
else
    _skip "PyQt6 not installed (GUI smoke test will skip)"
fi

HAS_PYYAML=0
if [[ -n "$PYTHON" ]] && "$PYTHON" -c "import yaml" 2>/dev/null; then
    _ok "PyYAML installed"
    HAS_PYYAML=1
else
    _skip "PyYAML not installed (config YAML tests will skip)"
fi

HAS_ARM_GCC=0
if command -v arm-none-eabi-gcc &>/dev/null; then
    _ok "arm-none-eabi-gcc: $(arm-none-eabi-gcc --version | head -1)"
    HAS_ARM_GCC=1
else
    _skip "arm-none-eabi-gcc not found (firmware build will skip)"
fi

# ── Python tool tests ─────────────────────────────────────────────────────────

_header "Python tool tests"

if [[ -z "$PYTHON" ]]; then
    _skip "python not found — skipping all tool tests"
else
    _run "pytest tool/tests/ (all)" \
        "$PYTHON" -m pytest tool/tests/ -q --tb=short

    _run "fake-target self-test (all 10 modes)" \
        "$PYTHON" -m tool.src.cli.bmsctl fake-target self-test
fi

# ── Config round-trip ─────────────────────────────────────────────────────────

_header "Config round-trip"

if [[ -n "$PYTHON" ]]; then
    TMP_DIR="$(mktemp -d)"
    trap 'rm -rf "$TMP_DIR"' EXIT

    _run "export default config" \
        "$PYTHON" -m tool.src.cli.bmsctl config export-default --out "$TMP_DIR/default.bin"

    _run "validate default config offline" \
        "$PYTHON" -m tool.src.cli.bmsctl config validate "$TMP_DIR/default.bin"

    _run "config export-json" \
        "$PYTHON" -m tool.src.cli.bmsctl config export-json "$TMP_DIR/default.bin" \
                  --out "$TMP_DIR/default.json"

    _run "config import-json round-trip" \
        "$PYTHON" -m tool.src.cli.bmsctl config import-json "$TMP_DIR/default.json" \
                  --out "$TMP_DIR/reimported.bin"

    _run "config diff (identical after round-trip)" \
        bash -c "$PYTHON -m tool.src.cli.bmsctl config diff \"$TMP_DIR/default.bin\" \
                 \"$TMP_DIR/reimported.bin\" | grep -q identical"

    if [[ "$HAS_PYYAML" -eq 1 ]]; then
        _run "config export-yaml" \
            "$PYTHON" -m tool.src.cli.bmsctl config export-yaml \
                      "$TMP_DIR/default.bin" --out "$TMP_DIR/default.yaml"
    else
        _skip "config export-yaml (PyYAML not installed)"
    fi
fi

# ── Package + update round-trip ───────────────────────────────────────────────

_header "Package + update round-trip"

if [[ -n "$PYTHON" ]]; then
    TMP_DIR2="$(mktemp -d)"
    trap 'rm -rf "$TMP_DIR2"' EXIT

    # Use build_firmware/firmware.bin if present, else synthesise a test binary
    if [[ -f "build_firmware/firmware.bin" ]]; then
        FW_SRC="build_firmware/firmware.bin"
        _ok "using real firmware.bin ($(wc -c < "$FW_SRC") bytes)"
    else
        python3 -c "import sys; sys.stdout.buffer.write(bytes(range(256))*32)" \
            > "$TMP_DIR2/fw.bin"
        FW_SRC="$TMP_DIR2/fw.bin"
        _ok "using synthetic 8 KB firmware"
    fi

    _run "package build" \
        "$PYTHON" -m tool.src.cli.bmsctl package build \
                  "$FW_SRC" "$TMP_DIR2/fw.pkg" --version 0.1.0

    _run "package inspect" \
        "$PYTHON" -m tool.src.cli.bmsctl package inspect "$TMP_DIR2/fw.pkg"

    _run "package validate" \
        "$PYTHON" -m tool.src.cli.bmsctl package validate "$TMP_DIR2/fw.pkg"

    _run "update dry-run" \
        "$PYTHON" -m tool.src.cli.bmsctl update dry-run "$TMP_DIR2/fw.pkg"

    # Start a fresh fake target for the simulate test
    VAL_PORT=65191
    "$PYTHON" -m tool.src.cli.bmsctl fake-target run --bind "127.0.0.1:$VAL_PORT" &
    _FT_PID=$!
    sleep 0.3
    _run "update simulate (fake target, healthy→bootloader)" \
        "$PYTHON" -m tool.src.cli.bmsctl update simulate \
                  "$TMP_DIR2/fw.pkg" --host 127.0.0.1 --port $VAL_PORT
    kill "$_FT_PID" 2>/dev/null; wait "$_FT_PID" 2>/dev/null || true

    _run "stlink dry-run-app" \
        "$PYTHON" -m tool.src.cli.bmsctl stlink dry-run-app "$TMP_DIR2/fw.pkg"
fi

# ── Firmware build ─────────────────────────────────────────────────────────────

_header "Firmware build"

if [[ "$SKIP_FIRMWARE" -eq 1 ]]; then
    _skip "firmware build (--no-firmware)"
elif [[ "$HAS_ARM_GCC" -eq 0 ]]; then
    _skip "firmware build (no arm-none-eabi-gcc)"
else
    _run "firmware build (STM32F303)" ./scripts/build_firmware.sh
fi

# ── Summary ───────────────────────────────────────────────────────────────────

echo
echo "══════════════════════════════════════════"
printf "  Results:  %d passed  %d failed  %d skipped\n" \
    "$PASS" "$FAIL" "$SKIPPED"
echo "══════════════════════════════════════════"

[[ "$FAIL" -eq 0 ]]
