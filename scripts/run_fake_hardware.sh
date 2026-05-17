#!/usr/bin/env bash
# run_fake_hardware.sh — Start the live fake-hardware simulator (evolving state).
#
# Unlike the static fake target (bmsctl fake-target run), this simulator runs a
# background tick thread that evolves cell voltages, temperatures, and uptime
# over real time — useful for GUI demos and time-series visualisation.
#
# Usage:
#   ./scripts/run_fake_hardware.sh                     # healthy-idle, port 65103
#   ./scripts/run_fake_hardware.sh --mode drive        # cells draining
#   ./scripts/run_fake_hardware.sh --mode cell-uv      # UV fault builds up
#   ./scripts/run_fake_hardware.sh --mode temp-high    # temps rising
#   ./scripts/run_fake_hardware.sh --port 65200        # custom port
#   ./scripts/run_fake_hardware.sh --seed 42           # deterministic drift
#
# Available modes (10 live modes):
#   healthy-idle       Cells ±5 mV random drift; no faults
#   drive              Cells slowly draining; pack current 50 A
#   charge             Cells slowly charging; pack current -30 A
#   cell-uv            cell[0] drifts toward undervoltage; fault triggers
#   cell-ov            cell[0] drifts toward overvoltage; fault triggers
#   temp-high          Temperatures rising to 45 °C plateau
#   isospi-fault       Static FAULT_ISOSPI_CELL; cells valid
#   openwire-detected  cell[0] flagged as open wire
#   vpack-invalid      Static FAULT_VPACK_INVALID
#   bootloader         Responds as FIRMWARE_TYPE_BOOTLOADER
#
# Connect the GUI:
#   ./scripts/run_gui.sh    then set host=127.0.0.1 port=65103 in Connection tab
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

if [[ -f "$REPO_ROOT/.venv/bin/activate" ]]; then
    # shellcheck source=/dev/null
    source "$REPO_ROOT/.venv/bin/activate"
fi

exec python3 -m tool.src.fake_target.live_simulator "$@"
