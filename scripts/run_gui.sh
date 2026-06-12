#!/usr/bin/env bash
# run_gui.sh — launch the BMS Tool desktop GUI.
#
# Usage:
#   ./scripts/run_gui.sh                               # open GUI, connect manually
#   ./scripts/run_gui.sh --fake                        # auto-start fake target + connect
#   ./scripts/run_gui.sh --fake --mode drive           # live simulation mode
#   ./scripts/run_gui.sh --fake --mode cell-uv         # static fault mode
#   ./scripts/run_gui.sh --fake --mode bootloader      # test update flow
#
# All unrecognised flags are passed through to the Python GUI entry point.
#
# Live --mode values (cells/temps/SOC evolve over time):
#   healthy-idle, drive, charge, cell-uv, cell-ov, temp-high,
#   isospi-fault, openwire-detected, vpack-invalid, bootloader
#
# Static --mode values (fixed snapshot):
#   healthy, safe_invalid, cell_uv, cell_ov, temp_invalid, vpack_invalid,
#   isospi_fault, config_error, overcurrent_fault, bootloader,
#   openwire_detected, openwire_pec_fail
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

# ── Activate .venv if present ─────────────────────────────────────────────────

VENV_ACTIVATE="$REPO_ROOT/.venv/bin/activate"
if [[ -f "$VENV_ACTIVATE" ]]; then
    # shellcheck source=/dev/null
    source "$VENV_ACTIVATE"
fi

# ── Python check ──────────────────────────────────────────────────────────────

PYTHON="${PYTHON:-python3}"
if ! command -v "$PYTHON" &>/dev/null; then
    echo "✗  python3 not found — run ./scripts/setup_dev_env.sh first" >&2
    exit 1
fi

# ── PyQt6 check ───────────────────────────────────────────────────────────────

if ! "$PYTHON" -c "import PyQt6" 2>/dev/null; then
    echo "✗  PyQt6 is not installed." >&2
    echo "" >&2
    echo "   Install it and re-run:" >&2
    echo "     pip install PyQt6" >&2
    echo "   or run setup first:" >&2
    echo "     ./scripts/setup_dev_env.sh" >&2
    exit 1
fi

# ── Kill any stale fake-target on the default port before --fake ──────────────

FAKE_PORT=65102
USES_FAKE=0
for arg in "$@"; do
    [[ "$arg" == "--fake" ]] && USES_FAKE=1
done

if [[ "$USES_FAKE" -eq 1 ]]; then
    STALE_PIDS="$(lsof -ti tcp:"$FAKE_PORT" 2>/dev/null || true)"
    if [[ -n "$STALE_PIDS" ]]; then
        echo "  Killing stale process on port $FAKE_PORT …"
        echo "$STALE_PIDS" | xargs kill 2>/dev/null || true
        sleep 0.2
    fi
fi

# ── Launch GUI ────────────────────────────────────────────────────────────────

echo "Launching BMS Tool …"
if [[ "$USES_FAKE" -eq 1 ]]; then
    echo "  Fake target will start in-process and auto-connect."
    echo "  Mode: $(echo "$*" | grep -oE '\-\-mode [^ ]+' | sed 's/--mode //' || echo 'healthy')"
fi
echo ""

exec "$PYTHON" -m tool.src.gui.main "$@"
