#!/usr/bin/env bash
# validate_all.sh — one-command validation of the full BMS repo.
#
# Usage:
#   ./scripts/validate_all.sh
#   ./scripts/validate_all.sh --no-firmware   # skip STM32 build (no toolchain needed)
#   ./scripts/validate_all.sh --no-c-tests    # skip C unit tests (no clang needed)
#
# Exit codes:
#   0  all sections passed
#   1  one or more sections failed
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_ROOT"

SKIP_FIRMWARE=0
SKIP_C_TESTS=0
for arg in "$@"; do
    [[ "$arg" == "--no-firmware" ]] && SKIP_FIRMWARE=1
    [[ "$arg" == "--no-c-tests"  ]] && SKIP_C_TESTS=1
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
        sed 's/^/    /' /tmp/validate_out.txt | head -40
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
    _skip "PyQt6 not installed (GUI smoke tests will skip)"
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

HAS_CLANG=0
if command -v clang &>/dev/null; then
    _ok "clang: $(clang --version | head -1)"
    HAS_CLANG=1
else
    _skip "clang not found (C unit tests will skip)"
fi

# ── Script syntax checks ──────────────────────────────────────────────────────

_header "Script syntax checks"

for script in \
    scripts/setup_dev_env.sh \
    scripts/run_gui.sh \
    scripts/bmsctl.sh \
    scripts/demo_local.sh \
    scripts/validate_all.sh \
    scripts/build_firmware.sh \
    scripts/build_bootloader.sh \
    scripts/package_release.sh \
    scripts/flash_stlink.sh \
    scripts/first_flash_dry_run.sh
do
    if [[ -f "$script" ]]; then
        _run "bash -n $script" bash -n "$script"
    else
        _skip "$script not found"
    fi
done

# ── Python tool tests ─────────────────────────────────────────────────────────

_header "Python tool tests"

if [[ -z "$PYTHON" ]]; then
    _skip "python not found — skipping all tool tests"
else
    _run "pytest tool/tests/ (all)" \
        "$PYTHON" -m pytest tool/tests/ -q --tb=short

    _run "fake-target self-test (all modes)" \
        "$PYTHON" -m tool.src.cli.bmsctl fake-target self-test
fi

# ── C unit tests ──────────────────────────────────────────────────────────────

_header "C unit tests"

if [[ "$SKIP_C_TESTS" -eq 1 ]]; then
    _skip "C unit tests (--no-c-tests)"
elif [[ "$HAS_CLANG" -eq 0 ]]; then
    _skip "C unit tests (clang not found)"
else
    _run "C unit tests (build_tests/run_tests.sh)" \
        bash build_tests/run_tests.sh
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

    if [[ -f "build_firmware/firmware.bin" ]]; then
        FW_SRC="build_firmware/firmware.bin"
        _ok "using real firmware.bin ($(wc -c < "$FW_SRC" | tr -d ' ') bytes)"
    else
        "$PYTHON" -c "import sys; sys.stdout.buffer.write(bytes(range(256))*32)" \
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

    _run "stlink dry-run-app" \
        "$PYTHON" -m tool.src.cli.bmsctl stlink dry-run-app "$TMP_DIR2/fw.pkg"

    # Start a fresh fake target for the simulate test
    VAL_PORT=65191
    "$PYTHON" -m tool.src.cli.bmsctl fake-target run \
        --bind "127.0.0.1:$VAL_PORT" >/dev/null 2>&1 &
    _FT_PID=$!
    sleep 0.3
    _run "update simulate (fake target, healthy→bootloader)" \
        "$PYTHON" -m tool.src.cli.bmsctl update simulate \
                  "$TMP_DIR2/fw.pkg" --host 127.0.0.1 --port $VAL_PORT
    kill "$_FT_PID" 2>/dev/null; wait "$_FT_PID" 2>/dev/null || true
fi

# ── Firmware build ─────────────────────────────────────────────────────────────

_header "Firmware build"

if [[ "$SKIP_FIRMWARE" -eq 1 ]]; then
    _skip "firmware build (--no-firmware)"
elif [[ "$HAS_ARM_GCC" -eq 0 ]]; then
    _skip "firmware build (no arm-none-eabi-gcc)"
else
    _run "firmware build (STM32F303)" ./scripts/build_firmware.sh
    if [[ -f "build_firmware/firmware.bin" ]]; then
        SIZE="$(wc -c < build_firmware/firmware.bin | tr -d ' ')"
        echo "    firmware.bin: ${SIZE} bytes"
    fi
fi

# ── Bootloader build ──────────────────────────────────────────────────────────

_header "Bootloader build (NOT hardware-validated)"

if [[ "$SKIP_FIRMWARE" -eq 1 ]]; then
    _skip "bootloader build (--no-firmware)"
elif [[ "$HAS_ARM_GCC" -eq 0 ]]; then
    _skip "bootloader build (no arm-none-eabi-gcc)"
else
    _run "bootloader build (STM32F303)" ./scripts/build_bootloader.sh
    if [[ -f "build_bootloader/bootloader.bin" ]]; then
        SIZE="$(wc -c < build_bootloader/bootloader.bin | tr -d ' ')"
        echo "    bootloader.bin: ${SIZE} bytes  [NOT HARDWARE-VALIDATED]"
    fi
fi

# ── Release bundle (smoke) ────────────────────────────────────────────────────

_header "Release bundle (smoke)"

if [[ -n "$PYTHON" ]]; then
    TMPREL="$(mktemp -d)"
    trap 'rm -rf "$TMPREL"' EXIT
    if ./scripts/package_release.sh --outdir "$TMPREL" --version validate-smoke \
            > /tmp/validate_out.txt 2>&1; then
        BUNDLE="$TMPREL/bms-vvalidate-smoke"
        if [[ -d "$BUNDLE" && -f "$BUNDLE/README.md" && -d "$BUNDLE/tool" ]]; then
            _ok "package_release.sh created bundle with README.md + tool/"
        else
            _fail "package_release.sh ran but bundle structure is wrong"
            ls "$BUNDLE" 2>/dev/null | sed 's/^/    /' | head -10
        fi
        # Check first-flash docs are in the bundle
        if [[ -f "$BUNDLE/docs/first_flash_guide.md" && \
              -f "$BUNDLE/docs/bench_safety_checklist.md" && \
              -f "$BUNDLE/docs/uart_smoke_test.md" && \
              -f "$BUNDLE/scripts/flash_stlink.sh" && \
              -f "$BUNDLE/manifest.txt" ]]; then
            _ok "bundle contains first-flash docs + flash_stlink.sh + manifest"
        else
            _fail "bundle missing first-flash docs or flash_stlink.sh or manifest.txt"
        fi
    else
        _fail "package_release.sh"
        sed 's/^/    /' /tmp/validate_out.txt | head -20
    fi
fi

# ── Flash dry-run (smoke) ─────────────────────────────────────────────────────

_header "Flash dry-run (smoke)"

if [[ -f "build_firmware/firmware.bin" ]]; then
    _run "flash_stlink.sh dry-run app" \
        bash scripts/flash_stlink.sh --app build_firmware/firmware.bin
else
    _skip "flash dry-run (build_firmware/firmware.bin not present)"
fi

# ── Summary ───────────────────────────────────────────────────────────────────

echo
echo "══════════════════════════════════════════"
printf "  Results:  %d passed  %d failed  %d skipped\n" \
    "$PASS" "$FAIL" "$SKIPPED"
echo "══════════════════════════════════════════"

[[ "$FAIL" -eq 0 ]]
