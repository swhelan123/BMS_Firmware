#!/usr/bin/env bash
# first_flash_dry_run.sh — pre-hardware readiness check.
#
# Runs the full validation suite, builds firmware, performs a flash dry-run,
# packages the release, and prints the first-flash next steps.
#
# Does NOT flash hardware.
#
# Usage:
#   ./scripts/first_flash_dry_run.sh
#   ./scripts/first_flash_dry_run.sh --no-firmware   # skip STM32 build
#   ./scripts/first_flash_dry_run.sh --no-c-tests    # skip C unit tests
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

SKIP_FIRMWARE=0
SKIP_C_TESTS=0
for arg in "$@"; do
    [[ "$arg" == "--no-firmware" ]] && SKIP_FIRMWARE=1
    [[ "$arg" == "--no-c-tests"  ]] && SKIP_C_TESTS=1
done

PASS=0
FAIL=0

_header() {
    echo
    echo "══════════════════════════════════════════"
    echo "  $1"
    echo "══════════════════════════════════════════"
}

_ok()   { echo "  ✓  $1"; PASS=$((PASS+1)); }
_fail() { echo "  ✗  $1"; FAIL=$((FAIL+1)); }

_run() {
    local label="$1"; shift
    if "$@" > /tmp/ffd_out.txt 2>&1; then
        _ok "$label"
    else
        _fail "$label"
        echo "    --- output ---"
        sed 's/^/    /' /tmp/ffd_out.txt | head -30
        echo "    ---"
    fi
}

# ── Validation suite ──────────────────────────────────────────────────────────

_header "Validation suite"

VALIDATE_ARGS=()
[[ "$SKIP_FIRMWARE" -eq 1 ]] && VALIDATE_ARGS+=("--no-firmware")
[[ "$SKIP_C_TESTS"  -eq 1 ]] && VALIDATE_ARGS+=("--no-c-tests")

_run "validate_all.sh" ./scripts/validate_all.sh ${VALIDATE_ARGS[@]+"${VALIDATE_ARGS[@]}"}

# ── Firmware build check ──────────────────────────────────────────────────────

_header "Firmware artifacts"

FW_BIN="$REPO_ROOT/build_firmware/firmware.bin"

if [[ "$SKIP_FIRMWARE" -eq 1 ]]; then
    echo "  ─  firmware build skipped (--no-firmware)"
    if [[ -f "$FW_BIN" ]]; then
        SIZE="$(wc -c < "$FW_BIN" | tr -d ' ')"
        _ok "firmware.bin present from prior build  ($SIZE bytes)"
    else
        echo "  ─  firmware.bin not found — flash dry-run will use placeholder"
    fi
else
    if [[ -f "$FW_BIN" ]]; then
        SIZE="$(wc -c < "$FW_BIN" | tr -d ' ')"
        _ok "firmware.bin  ($SIZE bytes)"
    else
        _fail "firmware.bin not found after validate_all.sh — firmware build may have been skipped"
    fi
fi

if [[ -f "$FW_BIN" ]]; then
    for artifact in firmware.hex bms_firmware.elf firmware.map; do
        if [[ -f "$REPO_ROOT/build_firmware/$artifact" ]]; then
            _ok "$artifact"
        else
            echo "  ─  $artifact  (not present)"
        fi
    done
fi

# ── Flash dry-run ─────────────────────────────────────────────────────────────

_header "Flash dry-run (no hardware)"

if [[ -f "$FW_BIN" ]]; then
    echo
    ./scripts/flash_stlink.sh --app "$FW_BIN" | sed 's/^/  /'
    echo
    _ok "flash_stlink.sh dry-run completed"
else
    echo "  ─  skipped (no firmware.bin)"
    echo "     Run ./scripts/build_firmware.sh first, or remove --no-firmware"
fi

# ── Release package ───────────────────────────────────────────────────────────

_header "Release package (smoke)"

TMPREL="$(mktemp -d)"
trap 'rm -rf "$TMPREL"' EXIT

if ./scripts/package_release.sh --outdir "$TMPREL" --version "dry-run" \
        > /tmp/ffd_out.txt 2>&1; then
    BUNDLE="$TMPREL/bms-vdry-run"
    MISSING=()
    [[ -f "$BUNDLE/README.md" ]]                          || MISSING+=("README.md")
    [[ -f "$BUNDLE/release_notes.md" ]]                   || MISSING+=("release_notes.md")
    [[ -f "$BUNDLE/manifest.txt" ]]                       || MISSING+=("manifest.txt")
    [[ -d "$BUNDLE/tool" ]]                               || MISSING+=("tool/")
    [[ -f "$BUNDLE/scripts/setup_dev_env.sh" ]]           || MISSING+=("scripts/setup_dev_env.sh")
    [[ -f "$BUNDLE/scripts/flash_stlink.sh" ]]            || MISSING+=("scripts/flash_stlink.sh")
    [[ -f "$BUNDLE/docs/first_flash_guide.md" ]]          || MISSING+=("docs/first_flash_guide.md")
    [[ -f "$BUNDLE/docs/bench_safety_checklist.md" ]]     || MISSING+=("docs/bench_safety_checklist.md")
    [[ -f "$BUNDLE/docs/uart_smoke_test.md" ]]            || MISSING+=("docs/uart_smoke_test.md")
    if [[ "${#MISSING[@]}" -eq 0 ]]; then
        _ok "bundle structure correct"
        MANIFEST_LINES="$(grep -v '^#' "$BUNDLE/manifest.txt" | grep -v '^$' | wc -l | tr -d ' ')"
        _ok "manifest.txt  ($MANIFEST_LINES files)"
    else
        _fail "bundle missing: ${MISSING[*]}"
        ls "$BUNDLE" | sed 's/^/    /'
    fi
else
    _fail "package_release.sh failed"
    sed 's/^/    /' /tmp/ffd_out.txt | head -20
fi

# ── Summary + next steps ──────────────────────────────────────────────────────

echo
echo "══════════════════════════════════════════"
printf "  Results:  %d passed  %d failed\n" "$PASS" "$FAIL"
echo "══════════════════════════════════════════"
echo

if [[ "$FAIL" -eq 0 ]]; then
    echo "  All checks passed. Repo is ready for first-flash preparation."
    echo
    echo "  Before hardware session:"
    echo
    echo "    1. Read docs/bench_safety_checklist.md"
    echo "    2. Read docs/first_flash_guide.md"
    echo "    3. Read docs/uart_smoke_test.md"
    echo "    4. Confirm hardware open questions in docs/01_hardware_contract.md §16"
    echo
    echo "  To flash (after bench setup is verified safe):"
    echo
    if [[ -f "$FW_BIN" ]]; then
        echo "    ./scripts/flash_stlink.sh --app build_firmware/firmware.bin --execute"
    else
        echo "    ./scripts/build_firmware.sh"
        echo "    ./scripts/flash_stlink.sh --app build_firmware/firmware.bin --execute"
    fi
    echo
    echo "  After flash (first CLI session):"
    echo
    echo "    PORT=/dev/tty.usbserial-XXXX    # replace with your port"
    echo "    ./scripts/bmsctl.sh connect  --serial \$PORT"
    echo "    ./scripts/bmsctl.sh diagnostics --serial \$PORT"
    echo "    ./scripts/bmsctl.sh diag gpio    --serial \$PORT"
    echo "    ./scripts/bmsctl.sh diag outputs --serial \$PORT"
    echo
else
    echo "  ${FAIL} check(s) failed — resolve before first-flash session."
    echo
fi

[[ "$FAIL" -eq 0 ]]
