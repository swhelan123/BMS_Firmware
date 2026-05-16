#!/usr/bin/env bash
# package_release.sh — build a release distribution bundle.
#
# Creates dist/bms-v{VERSION}/ containing:
#   firmware/           firmware.bin, firmware.hex, bms_firmware.elf, firmware.map
#                       firmware.pkg  (if package builder available and firmware.bin exists)
#   tool/               Python source (no .venv, no __pycache__, no .pytest_cache)
#   scripts/            setup_dev_env.sh, run_gui.sh, bmsctl.sh, demo_local.sh
#   docs/               all .md design documents
#   README.md
#   release_notes.md    auto-generated
#
# Options:
#   --outdir PATH       write bundle to PATH instead of dist/
#   --version VERSION   override version string (default: git describe or timestamp)
#
# Usage:
#   ./scripts/package_release.sh
#   ./scripts/package_release.sh --outdir /tmp/release
#   ./scripts/package_release.sh --version 1.0.0
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

# ── Args ──────────────────────────────────────────────────────────────────────

OUT_BASE="$REPO_ROOT/dist"
VERSION_OVERRIDE=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --outdir)   OUT_BASE="$2"; shift 2 ;;
        --version)  VERSION_OVERRIDE="$2"; shift 2 ;;
        *)          echo "Unknown option: $1" >&2; exit 1 ;;
    esac
done

# ── Version ───────────────────────────────────────────────────────────────────

if [[ -n "$VERSION_OVERRIDE" ]]; then
    VERSION="$VERSION_OVERRIDE"
elif VERSION="$(git -C "$REPO_ROOT" describe --tags --always 2>/dev/null)"; then
    : # already set
else
    VERSION="$(date +%Y%m%d_%H%M%S)"
fi

BUNDLE_NAME="bms-v${VERSION}"
BUNDLE_DIR="$OUT_BASE/$BUNDLE_NAME"

echo "==> BMS Release Bundle"
echo "    version:    $VERSION"
echo "    output:     $BUNDLE_DIR"

# ── Activate .venv if present ─────────────────────────────────────────────────

VENV_ACTIVATE="$REPO_ROOT/.venv/bin/activate"
if [[ -f "$VENV_ACTIVATE" ]]; then
    # shellcheck source=/dev/null
    source "$VENV_ACTIVATE"
fi

PYTHON="${PYTHON:-python3}"

# ── Setup output dir ──────────────────────────────────────────────────────────

mkdir -p "$BUNDLE_DIR"

# ── Firmware artifacts ────────────────────────────────────────────────────────

echo
echo "==> Firmware artifacts"

FW_DEST="$BUNDLE_DIR/firmware"
mkdir -p "$FW_DEST"

HAVE_FIRMWARE=0
for artifact in firmware.bin firmware.hex bms_firmware.elf firmware.map; do
    src="$REPO_ROOT/build_firmware/$artifact"
    if [[ -f "$src" ]]; then
        cp "$src" "$FW_DEST/$artifact"
        SIZE="$(wc -c < "$src" | tr -d ' ')"
        echo "    ✓  $artifact  ($SIZE bytes)"
        HAVE_FIRMWARE=1
    else
        echo "    ─  $artifact  (not found — run ./scripts/build_firmware.sh)"
    fi
done

# Build firmware.pkg if firmware.bin is available
if [[ "$HAVE_FIRMWARE" -eq 1 && -f "$REPO_ROOT/build_firmware/firmware.bin" ]]; then
    if "$PYTHON" -c "from tool.src.update.package_builder import build_package" 2>/dev/null; then
        PKG_OUT="$FW_DEST/firmware.pkg"
        # Package builder requires MAJOR.MINOR.PATCH — extract that from VERSION
        # Strip leading 'v', keep only the first semver-looking part
        PKG_VERSION="$(echo "$VERSION" | sed 's/^v//' | grep -oE '^[0-9]+\.[0-9]+\.[0-9]+' || echo "0.0.0")"
        if "$PYTHON" -m tool.src.cli.bmsctl package build \
            "$REPO_ROOT/build_firmware/firmware.bin" \
            "$PKG_OUT" \
            --version "$PKG_VERSION" 2>&1 | sed 's/^/    /'; then
            echo "    ✓  firmware.pkg  (v$PKG_VERSION)"
        else
            echo "    ─  firmware.pkg  (package build failed)"
        fi
    else
        echo "    ─  firmware.pkg  (package builder unavailable)"
    fi
fi

# ── Python tool ───────────────────────────────────────────────────────────────

echo
echo "==> Python tool (source)"

TOOL_DEST="$BUNDLE_DIR/tool"
rsync -a --quiet \
    --exclude='__pycache__' \
    --exclude='*.pyc' \
    --exclude='*.pyo' \
    --exclude='.pytest_cache' \
    --exclude='.venv' \
    --exclude='*.egg-info' \
    "$REPO_ROOT/tool/" "$TOOL_DEST/"
echo "    ✓  tool/  (Python source)"

# ── Scripts ───────────────────────────────────────────────────────────────────

echo
echo "==> Scripts"

SCRIPTS_DEST="$BUNDLE_DIR/scripts"
mkdir -p "$SCRIPTS_DEST"

for script in setup_dev_env.sh run_gui.sh bmsctl.sh demo_local.sh build_firmware.sh; do
    src="$REPO_ROOT/scripts/$script"
    if [[ -f "$src" ]]; then
        cp "$src" "$SCRIPTS_DEST/$script"
        chmod +x "$SCRIPTS_DEST/$script"
        echo "    ✓  scripts/$script"
    fi
done

# ── Docs ──────────────────────────────────────────────────────────────────────

echo
echo "==> Docs"

DOCS_DEST="$BUNDLE_DIR/docs"
mkdir -p "$DOCS_DEST"
if [[ -d "$REPO_ROOT/docs" ]]; then
    cp "$REPO_ROOT/docs/"*.md "$DOCS_DEST/" 2>/dev/null || true
    echo "    ✓  docs/*.md"
fi

# ── Root README ───────────────────────────────────────────────────────────────

if [[ -f "$REPO_ROOT/README.md" ]]; then
    cp "$REPO_ROOT/README.md" "$BUNDLE_DIR/README.md"
    echo
    echo "==> README.md"
    echo "    ✓  README.md"
fi

# ── Release notes ─────────────────────────────────────────────────────────────

echo
echo "==> Release notes"

GIT_LOG=""
if git -C "$REPO_ROOT" log --oneline -10 2>/dev/null; then
    GIT_LOG="$(git -C "$REPO_ROOT" log --oneline -10 2>/dev/null)"
fi

FW_SIZE_BYTES=""
if [[ -f "$REPO_ROOT/build_firmware/firmware.bin" ]]; then
    FW_SIZE_BYTES="$(wc -c < "$REPO_ROOT/build_firmware/firmware.bin" | tr -d ' ') bytes"
fi

PKG_VERSION_DISPLAY="$(echo "$VERSION" | sed 's/^v//' | grep -oE '^[0-9]+\.[0-9]+\.[0-9]+' || echo "$VERSION")"
cat > "$BUNDLE_DIR/release_notes.md" <<EOF
# BMS Release Notes — $VERSION

Generated: $(date -u '+%Y-%m-%d %H:%M UTC')

## Firmware

- Target: STM32F303VC (Cortex-M4, 256 KB flash)
- Flash layout: bootloader @ 0x08000000 (32 KB), app @ 0x08008000 (188 KB)
- Config A @ 0x08037000, Config B @ 0x08039000
${FW_SIZE_BYTES:+- Binary size: $FW_SIZE_BYTES}

## Status

- No hardware has been flashed with this build.
- All CLI and GUI workflows verified against the fake target simulator.
- Open an issue at the project repository before first-flash.

## Contents

\`\`\`
$BUNDLE_NAME/
  firmware/           ELF, .bin, .hex, .map${HAVE_FIRMWARE:+, .pkg}
  tool/               Python source (CLI + GUI + tests)
  scripts/            setup_dev_env.sh, run_gui.sh, bmsctl.sh, demo_local.sh
  docs/               Design documents
  README.md
  release_notes.md    This file
\`\`\`

## Quick Start

\`\`\`bash
cd $BUNDLE_NAME/
./scripts/setup_dev_env.sh        # install Python deps
./scripts/demo_local.sh           # full CLI demo (no hardware)
./scripts/run_gui.sh --fake       # GUI with fake target
\`\`\`

## Changelog (last 10 commits)

\`\`\`
$GIT_LOG
\`\`\`
EOF

echo "    ✓  release_notes.md"

# ── Summary ───────────────────────────────────────────────────────────────────

BUNDLE_SIZE="$(du -sh "$BUNDLE_DIR" | cut -f1)"

echo
echo "==> Bundle complete"
echo "    location:   $BUNDLE_DIR"
echo "    size:       $BUNDLE_SIZE"
echo
echo "    To create a tarball:"
echo "      tar -czf dist/${BUNDLE_NAME}.tar.gz -C \"$OUT_BASE\" \"$BUNDLE_NAME\""
echo
