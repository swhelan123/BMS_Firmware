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
    "flash_stlink.sh",
    "first_flash_dry_run.sh",
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


def test_package_release_includes_first_flash_docs(tmp_path):
    """Release bundle must include all first-flash documentation."""
    r = _run(
        [
            "bash", str(SCRIPTS / "package_release.sh"),
            "--outdir", str(tmp_path),
            "--version", "0.0.3",
        ],
        timeout=60,
    )
    assert r.returncode == 0, f"package_release.sh failed:\n{r.stdout}\n{r.stderr}"

    bundle = tmp_path / "bms-v0.0.3"
    assert (bundle / "docs" / "first_flash_guide.md").exists(), \
        "first_flash_guide.md missing from bundle"
    assert (bundle / "docs" / "bench_safety_checklist.md").exists(), \
        "bench_safety_checklist.md missing from bundle"
    assert (bundle / "docs" / "uart_smoke_test.md").exists(), \
        "uart_smoke_test.md missing from bundle"
    assert (bundle / "scripts" / "flash_stlink.sh").exists(), \
        "flash_stlink.sh missing from bundle"
    assert (bundle / "manifest.txt").exists(), \
        "manifest.txt missing from bundle"

    # manifest must not contain any build cache / generated junk
    manifest = (bundle / "manifest.txt").read_text()
    assert "__pycache__" not in manifest
    assert ".pytest_cache" not in manifest
    assert ".venv" not in manifest
    assert ".git" not in manifest


# ── flash_stlink.sh ───────────────────────────────────────────────────────────

def test_flash_stlink_dry_run_app(tmp_path):
    """flash_stlink.sh --app firmware.bin should dry-run and print the command."""
    fw_bin = REPO_ROOT / "build_firmware" / "firmware.bin"
    if not fw_bin.exists():
        pytest.skip("build_firmware/firmware.bin not present — skip flash dry-run test")

    r = _run(
        ["bash", str(SCRIPTS / "flash_stlink.sh"), "--app", str(fw_bin)],
        timeout=15,
    )
    assert r.returncode == 0, f"flash_stlink.sh dry-run failed:\n{r.stdout}\n{r.stderr}"
    combined = r.stdout + r.stderr
    assert "DRY-RUN" in combined or "dry-run" in combined.lower(), \
        "Expected DRY-RUN marker in output"
    assert "0x08008000" in combined, "Expected app start address in output"
    assert "STM32_Programmer_CLI" in combined, "Expected programmer command in output"


def test_flash_stlink_refuses_missing_file():
    """flash_stlink.sh must exit non-zero and print a clear error for a missing file."""
    r = _run(
        ["bash", str(SCRIPTS / "flash_stlink.sh"), "--app", "/nonexistent_firmware.bin"],
        timeout=10,
    )
    assert r.returncode != 0, "Expected non-zero exit for missing file"
    combined = r.stdout + r.stderr
    assert "not found" in combined or "No such" in combined, \
        "Expected 'not found' message in output"


def test_flash_stlink_refuses_oversized_file(tmp_path):
    """flash_stlink.sh must reject a .bin larger than the app region (188 KB)."""
    big_bin = tmp_path / "big.bin"
    big_bin.write_bytes(bytes(200 * 1024))  # 200 KB > 188 KB app region

    r = _run(
        ["bash", str(SCRIPTS / "flash_stlink.sh"), "--app", str(big_bin)],
        timeout=10,
    )
    assert r.returncode != 0, "Expected non-zero exit for oversized file"
    combined = r.stdout + r.stderr
    assert "exceeds" in combined or "too large" in combined.lower() or "bytes" in combined, \
        "Expected size-exceeded message in output"


def test_flash_stlink_requires_execute_for_bootloader(tmp_path):
    """flash_stlink.sh --bootloader without --execute must refuse."""
    dummy = tmp_path / "bl.bin"
    dummy.write_bytes(bytes(64))

    r = _run(
        ["bash", str(SCRIPTS / "flash_stlink.sh"), "--bootloader", str(dummy)],
        timeout=10,
    )
    assert r.returncode != 0, "Expected non-zero exit for --bootloader without --execute"
    combined = r.stdout + r.stderr
    assert "--execute" in combined, "Expected --execute mentioned in error"


def test_flash_stlink_dry_run_does_not_invoke_programmer():
    """Dry-run must not call the actual programmer (it won't be present in CI)."""
    fw_bin = REPO_ROOT / "build_firmware" / "firmware.bin"
    if not fw_bin.exists():
        pytest.skip("build_firmware/firmware.bin not present")

    r = _run(
        ["bash", str(SCRIPTS / "flash_stlink.sh"), "--app", str(fw_bin)],
        timeout=15,
    )
    # dry-run exits 0 even without programmer installed
    assert r.returncode == 0
    # programmer was not actually invoked (no "Download verified" from real flash)
    assert "Download verified" not in (r.stdout + r.stderr)


# ── first_flash_dry_run.sh ────────────────────────────────────────────────────

def test_first_flash_dry_run_is_executable():
    """first_flash_dry_run.sh must exist and be executable."""
    path = SCRIPTS / "first_flash_dry_run.sh"
    assert path.exists(), "first_flash_dry_run.sh not found"
    import os
    assert os.access(path, os.X_OK), "first_flash_dry_run.sh is not executable"


def test_first_flash_dry_run_prints_next_steps():
    """first_flash_dry_run.sh --help-only shows first-flash next-step hints."""
    # We only run a partial check here because first_flash_dry_run.sh calls
    # validate_all.sh which itself runs pytest — running it in full from pytest
    # creates circular recursion.  validate_all.sh smoke-tests this script instead.
    # Here we just confirm the script passes syntax checking and contains expected content.
    content = (SCRIPTS / "first_flash_dry_run.sh").read_text()
    assert "flash_stlink.sh" in content, "should reference flash_stlink.sh"
    assert "bmsctl.sh" in content, "should reference bmsctl"
    assert "bench_safety_checklist" in content, "should reference bench_safety_checklist"
    assert "first_flash_guide" in content, "should reference first_flash_guide"
