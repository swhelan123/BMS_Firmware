"""test_scripts.py — validate shell scripts and wrapper behavior.

Tests:
  - Syntax check all scripts (bash -n)
  - bmsctl.sh --help works
  - bmsctl.sh connect fails gracefully against a closed port
  - demo_local.sh non-GUI path runs end-to-end
  - package_release.sh creates expected dist structure
"""
import os
import subprocess
import sys
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).parent.parent.parent
SCRIPTS   = REPO_ROOT / "scripts"


# ── helpers ───────────────────────────────────────────────────────────────────

def _bash_n(path: Path):
    r = subprocess.run(["bash", "-n", str(path)], capture_output=True, text=True)
    return r.returncode == 0, r.stderr.strip()


def _run(cmd, timeout=60, **kwargs):
    return subprocess.run(
        cmd, capture_output=True, text=True, timeout=timeout,
        cwd=REPO_ROOT, **kwargs,
    )


# ── syntax checks ─────────────────────────────────────────────────────────────

@pytest.mark.parametrize("script", [
    "setup_dev_env.sh",
    "run_gui.sh",
    "bmsctl.sh",
    "demo_local.sh",
    "validate_all.sh",
    "build_firmware.sh",
    "package_release.sh",
])
def test_script_syntax(script):
    path = SCRIPTS / script
    assert path.exists(), f"{script} not found"
    ok, err = _bash_n(path)
    assert ok, f"bash -n {script} failed:\n{err}"


# ── bmsctl.sh wrapper ─────────────────────────────────────────────────────────

def test_bmsctl_wrapper_is_executable():
    path = SCRIPTS / "bmsctl.sh"
    assert path.exists()
    assert os.access(path, os.X_OK), "bmsctl.sh is not executable"


def test_bmsctl_wrapper_help():
    r = _run([str(SCRIPTS / "bmsctl.sh"), "--help"])
    assert r.returncode == 0, f"bmsctl --help failed:\n{r.stderr}"
    combined = r.stdout + r.stderr
    assert "bmsctl" in combined.lower() or "usage" in combined.lower()


def test_bmsctl_wrapper_connect_refused():
    """Connect against a guaranteed-closed port must fail (not crash with traceback)."""
    r = _run(
        [str(SCRIPTS / "bmsctl.sh"), "connect", "--port", "19999"],
        timeout=8,
    )
    assert r.returncode != 0, "Expected non-zero exit for refused connection"
    # Should not be an unhandled Python traceback reaching the user
    assert "Traceback" not in r.stdout
    assert "Traceback" not in r.stderr


def test_bmsctl_wrapper_fake_target_self_test():
    """fake-target self-test covers all modes — should exit 0."""
    r = _run(
        [str(SCRIPTS / "bmsctl.sh"), "fake-target", "self-test"],
        timeout=30,
    )
    assert r.returncode == 0, (
        f"fake-target self-test failed:\n{r.stdout}\n{r.stderr}"
    )


# ── demo_local.sh ─────────────────────────────────────────────────────────────

def test_demo_local_no_gui_exits_zero():
    """demo_local.sh without --gui should complete and exit 0."""
    r = _run(
        ["bash", str(SCRIPTS / "demo_local.sh"), "--skip-update"],
        timeout=60,
    )
    assert r.returncode == 0, (
        f"demo_local.sh failed:\n--- stdout ---\n{r.stdout}\n"
        f"--- stderr ---\n{r.stderr}"
    )
    # Sanity: demo prints its completion header
    assert "Demo complete" in r.stdout or "Demo complete" in r.stderr


# ── package_release.sh ────────────────────────────────────────────────────────

def test_package_release_creates_bundle(tmp_path):
    """package_release.sh should create a dist bundle with expected structure."""
    r = _run(
        [
            "bash", str(SCRIPTS / "package_release.sh"),
            "--outdir", str(tmp_path),
            "--version", "0.0.1",
        ],
        timeout=60,
    )
    assert r.returncode == 0, (
        f"package_release.sh failed:\n{r.stdout}\n{r.stderr}"
    )

    bundle = tmp_path / "bms-v0.0.1"
    assert bundle.is_dir(), f"Bundle dir not created: {bundle}"

    # Required files / dirs
    assert (bundle / "README.md").exists(),            "README.md missing"
    assert (bundle / "release_notes.md").exists(),     "release_notes.md missing"
    assert (bundle / "scripts" / "setup_dev_env.sh").exists()
    assert (bundle / "scripts" / "run_gui.sh").exists()
    assert (bundle / "scripts" / "bmsctl.sh").exists()
    assert (bundle / "scripts" / "demo_local.sh").exists()
    assert (bundle / "tool").is_dir(),                 "tool/ missing"
    assert (bundle / "tool" / "src").is_dir(),         "tool/src/ missing"

    # No pycache should be in the bundle
    pycaches = list(bundle.rglob("__pycache__"))
    assert not pycaches, f"__pycache__ found in bundle: {pycaches}"

    # No .venv in the bundle
    assert not (bundle / ".venv").exists(), ".venv should not be in bundle"


def test_package_release_firmware_dir(tmp_path):
    """If build_firmware artifacts exist, they must be copied into the bundle."""
    fw_bin = REPO_ROOT / "build_firmware" / "firmware.bin"
    if not fw_bin.exists():
        pytest.skip("build_firmware/firmware.bin not present — skip firmware artifact check")

    r = _run(
        [
            "bash", str(SCRIPTS / "package_release.sh"),
            "--outdir", str(tmp_path),
            "--version", "0.0.2",
        ],
        timeout=60,
    )
    assert r.returncode == 0

    bundle = tmp_path / "bms-v0.0.2"
    assert (bundle / "firmware" / "firmware.bin").exists()
    assert (bundle / "firmware" / "firmware.hex").exists()
    assert (bundle / "firmware" / "bms_firmware.elf").exists()
