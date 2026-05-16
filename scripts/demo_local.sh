#!/usr/bin/env bash
# demo_local.sh — full BMS stack demo without real hardware.
#
# Starts a fake target server, runs CLI operations against it, then stops.
# Pass --gui to also launch the desktop GUI connected to the fake target.
#
# Usage:
#   ./scripts/demo_local.sh
#   ./scripts/demo_local.sh --gui
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_ROOT"

GUI_MODE=0
for arg in "$@"; do
    [[ "$arg" == "--gui" ]] && GUI_MODE=1
done

PYTHON="${PYTHON:-python3}"

# ── Port allocation ───────────────────────────────────────────────────────────

DEMO_PORT=65210

_header() {
    echo
    echo "────────────────────────────────────────"
    echo "  $1"
    echo "────────────────────────────────────────"
}

_step() { echo "  » $1"; }
_ok()   { echo "  ✓  $1"; }

# ── Start fake target ─────────────────────────────────────────────────────────

_header "Starting fake target (healthy mode)"

"$PYTHON" -m tool.src.cli.bmsctl fake-target run \
    --mode healthy --bind "127.0.0.1:$DEMO_PORT" &
FT_PID=$!
trap 'kill $FT_PID 2>/dev/null; echo; echo "  fake target stopped."' EXIT
sleep 0.4  # give server a moment to bind

_ok "fake target running on port $DEMO_PORT (PID $FT_PID)"

CARGS="--host 127.0.0.1 --port $DEMO_PORT"

# ── Connect ───────────────────────────────────────────────────────────────────

_header "Connect"
"$PYTHON" -m tool.src.cli.bmsctl connect $CARGS
_ok "connected"

# ── Read measurements ─────────────────────────────────────────────────────────

_header "Measurements"

_step "values"
"$PYTHON" -m tool.src.cli.bmsctl values $CARGS

_step "cells (summary)"
"$PYTHON" -m tool.src.cli.bmsctl cells $CARGS

_step "cells (verbose — first 5 lines)"
"$PYTHON" -m tool.src.cli.bmsctl cells -v $CARGS | head -7

_step "temperatures"
"$PYTHON" -m tool.src.cli.bmsctl temps $CARGS

_step "faults (should be zero)"
"$PYTHON" -m tool.src.cli.bmsctl faults $CARGS

_step "diagnostics"
"$PYTHON" -m tool.src.cli.bmsctl diagnostics $CARGS

# ── Config ────────────────────────────────────────────────────────────────────

_header "Config"

TMP_DIR="$(mktemp -d)"
trap 'rm -rf "$TMP_DIR"; kill $FT_PID 2>/dev/null' EXIT

_step "read config from target"
"$PYTHON" -m tool.src.cli.bmsctl config read --out "$TMP_DIR/target.bin" $CARGS

_step "export default config to JSON"
"$PYTHON" -m tool.src.cli.bmsctl config export-json "$TMP_DIR/target.bin" \
    --out "$TMP_DIR/config.json"
echo "  → saved to $TMP_DIR/config.json"
head -8 "$TMP_DIR/config.json"

_step "apply default config to RAM"
"$PYTHON" -m tool.src.cli.bmsctl config export-default --out "$TMP_DIR/default.bin"
"$PYTHON" -m tool.src.cli.bmsctl config apply-ram "$TMP_DIR/default.bin" $CARGS

# ── Package + update ──────────────────────────────────────────────────────────

_header "Package + firmware update simulation"

# Use real firmware.bin if built, else synthesise
if [[ -f "build_firmware/firmware.bin" ]]; then
    FW_SRC="build_firmware/firmware.bin"
    _ok "using build_firmware/firmware.bin"
else
    "$PYTHON" -c "import sys; sys.stdout.buffer.write(bytes(range(256))*32)" \
        > "$TMP_DIR/fw.bin"
    FW_SRC="$TMP_DIR/fw.bin"
    _ok "using synthetic 8 KB firmware"
fi

_step "build .pkg"
"$PYTHON" -m tool.src.cli.bmsctl package build \
    "$FW_SRC" "$TMP_DIR/fw.pkg" --version 0.1.0

_step "inspect package"
"$PYTHON" -m tool.src.cli.bmsctl package inspect "$TMP_DIR/fw.pkg"

_step "validate package"
"$PYTHON" -m tool.src.cli.bmsctl package validate "$TMP_DIR/fw.pkg"

_step "update dry-run"
"$PYTHON" -m tool.src.cli.bmsctl update dry-run "$TMP_DIR/fw.pkg"

_step "update simulate (enter bootloader → flash → done)"
"$PYTHON" -m tool.src.cli.bmsctl update simulate "$TMP_DIR/fw.pkg" $CARGS

# ── GUI (optional) ────────────────────────────────────────────────────────────

if [[ "$GUI_MODE" -eq 1 ]]; then
    _header "Launching GUI (--fake --mode healthy)"
    _ok "close the window to end the demo"
    "$PYTHON" -m tool.src.gui.main --fake --mode healthy
fi

_header "Demo complete"
