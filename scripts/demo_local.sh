#!/usr/bin/env bash
# demo_local.sh — full BMS stack demo without real hardware.
#
# Starts a fake-target TCP server, runs CLI operations against it,
# optionally simulates a firmware update, then stops.
#
# Usage:
#   ./scripts/demo_local.sh
#   ./scripts/demo_local.sh --gui              # also launch GUI at the end
#   ./scripts/demo_local.sh --skip-update      # skip update simulation (faster)
#
# Output: colour-coded commands + results; fake-target log suppressed.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

# ── Args ──────────────────────────────────────────────────────────────────────

GUI_MODE=0
SKIP_UPDATE=0
for arg in "$@"; do
    [[ "$arg" == "--gui"         ]] && GUI_MODE=1
    [[ "$arg" == "--skip-update" ]] && SKIP_UPDATE=1
done

# ── Activate .venv if present ─────────────────────────────────────────────────

VENV_ACTIVATE="$REPO_ROOT/.venv/bin/activate"
if [[ -f "$VENV_ACTIVATE" ]]; then
    # shellcheck source=/dev/null
    source "$VENV_ACTIVATE"
fi

PYTHON="${PYTHON:-python3}"

# ── Helpers ───────────────────────────────────────────────────────────────────

DEMO_PORT=65210
FT_LOG="$(mktemp /tmp/bms_fake_target.XXXXXX.log)"
TMP_DIR="$(mktemp -d)"

_header() {
    echo
    printf '\033[1;36m────────────────────────────────────────\033[0m\n'
    printf '\033[1;36m  %s\033[0m\n' "$1"
    printf '\033[1;36m────────────────────────────────────────\033[0m\n'
}

_cmd() {   # print the command in dim grey
    printf '  \033[2m$ %s\033[0m\n' "$*"
}

_run() {   # _run label cmd…
    local label="$1"; shift
    _cmd "$*"
    "$@" 2>&1 | sed 's/^/    /'
    printf '  \033[32m✓  %s\033[0m\n' "$label"
}

_ok()   { printf '  \033[32m✓  %s\033[0m\n' "$1"; }
_info() { printf '     %s\n' "$1"; }

CARGS="--host 127.0.0.1 --port $DEMO_PORT"

# ── Cleanup trap ──────────────────────────────────────────────────────────────

cleanup() {
    if [[ -n "${FT_PID:-}" ]]; then
        kill "$FT_PID" 2>/dev/null || true
        wait "$FT_PID" 2>/dev/null || true
    fi
    rm -f "$FT_LOG"
    rm -rf "$TMP_DIR"
}
trap cleanup EXIT

# ── Start fake target ─────────────────────────────────────────────────────────

_header "Starting fake target (healthy mode, port $DEMO_PORT)"

_cmd "python3 -m tool.src.cli.bmsctl fake-target run --mode healthy --bind 127.0.0.1:$DEMO_PORT &"
"$PYTHON" -m tool.src.cli.bmsctl fake-target run \
    --mode healthy --bind "127.0.0.1:$DEMO_PORT" >"$FT_LOG" 2>&1 &
FT_PID=$!
sleep 0.4
_ok "fake target listening on 127.0.0.1:$DEMO_PORT  (PID $FT_PID)"

# ── Connect ───────────────────────────────────────────────────────────────────

_header "Connect + capabilities"
_run "connect" "$PYTHON" -m tool.src.cli.bmsctl connect $CARGS

# ── Measurements ─────────────────────────────────────────────────────────────

_header "Pack values"
_run "values" "$PYTHON" -m tool.src.cli.bmsctl values $CARGS

_header "Cell voltages"
_run "cells (summary)" "$PYTHON" -m tool.src.cli.bmsctl cells $CARGS
echo "    (first 3 cells):"
_cmd "python3 -m tool.src.cli.bmsctl cells -v $CARGS | head -9"
"$PYTHON" -m tool.src.cli.bmsctl cells -v $CARGS 2>&1 | head -9 | sed 's/^/    /'

_header "Temperatures"
_run "temps" "$PYTHON" -m tool.src.cli.bmsctl temps $CARGS

_header "Faults (all zero — healthy mode)"
_run "faults" "$PYTHON" -m tool.src.cli.bmsctl faults $CARGS

_header "Diagnostics counters"
_run "diagnostics" "$PYTHON" -m tool.src.cli.bmsctl diagnostics $CARGS

_header "Open-wire detection"
_run "openwire run" "$PYTHON" -m tool.src.cli.bmsctl openwire run $CARGS

# ── Config ────────────────────────────────────────────────────────────────────

_header "Config"
_run "read config from target" \
    "$PYTHON" -m tool.src.cli.bmsctl config read --out "$TMP_DIR/target.bin" $CARGS

_run "export JSON" \
    "$PYTHON" -m tool.src.cli.bmsctl config export-json \
        "$TMP_DIR/target.bin" --out "$TMP_DIR/config.json"

_info "(first 6 lines of config.json:)"
head -6 "$TMP_DIR/config.json" | sed 's/^/    /'

_run "export default config" \
    "$PYTHON" -m tool.src.cli.bmsctl config export-default \
        --out "$TMP_DIR/default.bin"

_run "validate default config offline" \
    "$PYTHON" -m tool.src.cli.bmsctl config validate "$TMP_DIR/default.bin"

_run "apply default config to RAM" \
    "$PYTHON" -m tool.src.cli.bmsctl config apply-ram \
        "$TMP_DIR/default.bin" $CARGS

# ── Package + firmware update ─────────────────────────────────────────────────

_header "Firmware package"

if [[ -f "$REPO_ROOT/build_firmware/firmware.bin" ]]; then
    FW_SRC="$REPO_ROOT/build_firmware/firmware.bin"
    FW_SIZE="$(wc -c < "$FW_SRC" | tr -d ' ')"
    _ok "using build_firmware/firmware.bin  ($FW_SIZE bytes)"
else
    "$PYTHON" -c "import sys; sys.stdout.buffer.write(bytes(range(256))*32)" \
        > "$TMP_DIR/fw_synth.bin"
    FW_SRC="$TMP_DIR/fw_synth.bin"
    _info "Note: no firmware.bin found — using synthetic 8 KB binary"
    _info "(run ./scripts/build_firmware.sh to build real firmware)"
fi

_run "package build" \
    "$PYTHON" -m tool.src.cli.bmsctl package build \
        "$FW_SRC" "$TMP_DIR/fw.pkg" --version 0.1.0

_run "package inspect" \
    "$PYTHON" -m tool.src.cli.bmsctl package inspect "$TMP_DIR/fw.pkg"

_run "package validate" \
    "$PYTHON" -m tool.src.cli.bmsctl package validate "$TMP_DIR/fw.pkg"

_run "update dry-run (no target needed)" \
    "$PYTHON" -m tool.src.cli.bmsctl update dry-run "$TMP_DIR/fw.pkg"

if [[ "$SKIP_UPDATE" -eq 0 ]]; then
    _header "Firmware update simulation (enter-bootloader → BEGIN → chunks → FINALIZE)"
    _run "update simulate" \
        "$PYTHON" -m tool.src.cli.bmsctl update simulate \
            "$TMP_DIR/fw.pkg" $CARGS
else
    _info "(update simulation skipped — pass without --skip-update to include)"
fi

# ── GUI (optional) ────────────────────────────────────────────────────────────

if [[ "$GUI_MODE" -eq 1 ]]; then
    if ! "$PYTHON" -c "import PyQt6" 2>/dev/null; then
        _header "GUI"
        printf '  \033[33m⚠  PyQt6 not installed — GUI skipped.\033[0m\n'
        _info "Install: pip install PyQt6"
    else
        _header "Launching GUI (fake target, healthy mode)"
        _info "Close the window to end the demo."
        _cmd "python3 -m tool.src.gui.main --fake --mode healthy"
        "$PYTHON" -m tool.src.gui.main --fake --mode healthy
    fi
fi

# ── Done ──────────────────────────────────────────────────────────────────────

_header "Demo complete"
_ok "All steps passed."
echo
_info "Next steps:"
_info "  ./scripts/run_gui.sh --fake --mode healthy      # GUI with fake target"
_info "  ./scripts/run_gui.sh --fake --mode openwire_detected"
_info "  ./scripts/validate_all.sh                       # full validation suite"
_info "  ./scripts/package_release.sh                    # build release bundle"
echo
