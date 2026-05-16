#!/usr/bin/env bash
# setup_dev_env.sh — one-command dev environment setup.
#
# Creates .venv, installs Python dependencies, checks build tools.
#
# Usage:
#   ./scripts/setup_dev_env.sh
#   ./scripts/setup_dev_env.sh --no-venv    # skip venv create/install
#
# Exit 0 if required tools are present; exits with a warning summary if only
# optional tools are missing (e.g. STM32_Programmer_CLI).
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

# ── Arg parsing ───────────────────────────────────────────────────────────────

NO_VENV=0
for arg in "$@"; do
    [[ "$arg" == "--no-venv" ]] && NO_VENV=1
done

# ── Helpers ───────────────────────────────────────────────────────────────────

_ok()   { printf "  ✓  %s\n" "$1"; }
_warn() { printf "  ⚠  %s\n" "$1"; }
_fail() { printf "  ✗  %s\n" "$1"; }
_info() { printf "     %s\n" "$1"; }
_sep()  { echo "─────────────────────────────────────────────"; }

MISSING_REQUIRED=()
MISSING_OPTIONAL=()

_require() { # _require "label" "check_cmd"
    if eval "$2" &>/dev/null; then
        _ok "$1"
    else
        _fail "$1"
        MISSING_REQUIRED+=("$1")
    fi
}

_optional() { # _optional "label" "check_cmd" "install_hint"
    if eval "$2" &>/dev/null; then
        _ok "$1"
    else
        _warn "$1 not found  →  $3"
        MISSING_OPTIONAL+=("$1")
    fi
}

# ── Python ─────────────────────────────────────────────────────────────────────

echo
_sep
echo "  Python"
_sep

# Find a Python 3.11+ interpreter
PYTHON=""
for py in python3.13 python3.12 python3.11 python3 python; do
    if command -v "$py" &>/dev/null; then
        ver="$("$py" -c 'import sys; print(sys.version_info[:2])')"
        if "$py" -c "import sys; assert sys.version_info>=(3,11)" 2>/dev/null; then
            PYTHON="$py"
            _ok "Python $("$py" --version 2>&1)  →  $py"
            break
        fi
    fi
done

if [[ -z "$PYTHON" ]]; then
    _fail "Python 3.11+ not found"
    _info "Install from https://www.python.org/downloads/ or via brew:"
    _info "  brew install python@3.12"
    MISSING_REQUIRED+=("python3.11+")
fi

# ── Virtual environment ────────────────────────────────────────────────────────

echo
_sep
echo "  Virtual environment (.venv)"
_sep

if [[ "$NO_VENV" -eq 1 ]]; then
    _warn "skipped (--no-venv)"
elif [[ -z "$PYTHON" ]]; then
    _warn "skipped (no Python 3.11+)"
else
    VENV_DIR="$REPO_ROOT/.venv"
    if [[ -d "$VENV_DIR" ]]; then
        _ok ".venv already exists"
    else
        echo "  Creating $VENV_DIR …"
        "$PYTHON" -m venv "$VENV_DIR"
        _ok ".venv created"
    fi

    # Activate and install
    # shellcheck source=/dev/null
    source "$VENV_DIR/bin/activate"
    echo "  Installing tool/requirements.txt …"
    pip install --quiet --upgrade pip
    pip install --quiet -r "$REPO_ROOT/tool/requirements.txt"
    _ok "Python packages installed"

    # Verify key imports
    for pkg in serial yaml pytest; do
        if python3 -c "import $pkg" 2>/dev/null; then
            _ok "  import $pkg  ✓"
        else
            _warn "  import $pkg failed — package install may have issues"
        fi
    done

    # PyQt6 is optional (GUI only)
    if python3 -c "import PyQt6" 2>/dev/null; then
        _ok "  import PyQt6  ✓  (GUI available)"
    else
        _warn "  PyQt6 not importable — GUI will be unavailable"
        _info "  This is expected in headless CI environments."
        _info "  On a desktop: pip install PyQt6"
        MISSING_OPTIONAL+=("PyQt6 (GUI only)")
    fi
fi

# ── Firmware build tools ───────────────────────────────────────────────────────

echo
_sep
echo "  Firmware build tools"
_sep

if command -v arm-none-eabi-gcc &>/dev/null; then
    _ok "arm-none-eabi-gcc  $(arm-none-eabi-gcc --version | head -1)"
else
    _warn "arm-none-eabi-gcc not found"
    _info "Download: https://developer.arm.com/downloads/-/arm-gnu-toolchain-downloads"
    _info "macOS:    brew install --cask gcc-arm-embedded"
    _info "Typical:  export PATH=\"/Applications/ArmGNUToolchain/15.2.rel1/arm-none-eabi/bin:\$PATH\""
    MISSING_OPTIONAL+=("arm-none-eabi-gcc (firmware build)")
fi

if command -v cmake &>/dev/null; then
    _ok "cmake  $(cmake --version | head -1)"
else
    _warn "cmake not found"
    _info "macOS: brew install cmake"
    MISSING_OPTIONAL+=("cmake (firmware build)")
fi

if command -v ninja &>/dev/null; then
    _ok "ninja  $(ninja --version)"
else
    _warn "ninja not found"
    _info "macOS: brew install ninja"
    MISSING_OPTIONAL+=("ninja (firmware build)")
fi

# C unit test runner uses clang
if command -v clang &>/dev/null; then
    _ok "clang  $(clang --version | head -1)"
else
    _warn "clang not found — C unit tests (build_tests/run_tests.sh) will fail"
    MISSING_OPTIONAL+=("clang (C unit tests)")
fi

# ── Optional tools ─────────────────────────────────────────────────────────────

echo
_sep
echo "  Optional tools"
_sep

if command -v STM32_Programmer_CLI &>/dev/null; then
    _ok "STM32_Programmer_CLI  (ST-Link flash available)"
else
    _warn "STM32_Programmer_CLI not found"
    _info "Not needed for simulation/testing.  Required only to flash real hardware."
    _info "Download: https://www.st.com/en/development-tools/stm32cubeprog.html"
    MISSING_OPTIONAL+=("STM32_Programmer_CLI (ST-Link flash only)")
fi

# ── Summary ────────────────────────────────────────────────────────────────────

echo
_sep
echo "  Summary"
_sep

if [[ ${#MISSING_REQUIRED[@]} -gt 0 ]]; then
    echo
    _fail "Missing required tools — please install before continuing:"
    for t in "${MISSING_REQUIRED[@]}"; do
        _info "  • $t"
    done
    echo
    exit 1
fi

if [[ ${#MISSING_OPTIONAL[@]} -gt 0 ]]; then
    echo
    _warn "Missing optional tools (simulation + testing still work without them):"
    for t in "${MISSING_OPTIONAL[@]}"; do
        _info "  • $t"
    done
fi

echo
_ok "Setup complete."
echo
echo "  Quick-start:"
echo "    ./scripts/validate_all.sh        # validate everything"
echo "    ./scripts/demo_local.sh          # full stack demo"
echo "    ./scripts/run_gui.sh --fake      # launch GUI with fake target"
echo "    ./scripts/bmsctl.sh --help       # CLI help"
echo
