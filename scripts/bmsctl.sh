#!/usr/bin/env bash
# bmsctl.sh — thin wrapper that activates .venv and runs bmsctl.
#
# Usage:
#   ./scripts/bmsctl.sh --help
#   ./scripts/bmsctl.sh connect
#   ./scripts/bmsctl.sh values --json
#   ./scripts/bmsctl.sh --host 192.168.1.100 --port 65102 cells -v
#
# Any argument is passed through to tool.src.cli.bmsctl unchanged.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

# Activate .venv if it exists
VENV_ACTIVATE="$REPO_ROOT/.venv/bin/activate"
if [[ -f "$VENV_ACTIVATE" ]]; then
    # shellcheck source=/dev/null
    source "$VENV_ACTIVATE"
fi

PYTHON="${PYTHON:-python3}"
if ! command -v "$PYTHON" &>/dev/null; then
    echo "✗  python3 not found — run ./scripts/setup_dev_env.sh first" >&2
    exit 1
fi

exec "$PYTHON" -m tool.src.cli.bmsctl "$@"
