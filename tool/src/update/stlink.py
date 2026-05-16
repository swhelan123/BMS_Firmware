"""stlink.py — STM32_Programmer_CLI wrapper for BMS firmware flashing.

Safety rules enforced here:
  - dry_run_app() generates and returns the command; it never executes.
  - execute_flash() requires confirm=True as an explicit safety gate.
  - Raw binary without explicit flash address is refused.
  - Package metadata supplies the flash address — no guessing.

Do not call execute_flash() from the GUI unless the user has checked
an explicit "I understand this will flash hardware" checkbox.
"""
import shutil
import subprocess
from pathlib import Path
from typing import List, Optional, Tuple

from .package_parser import parse_and_validate_package, PackageValidationError

PROGRAMMER_NAMES = ['STM32_Programmer_CLI', 'STM32_Programmer_CLI.exe']


def detect_programmer() -> Optional[str]:
    """Return the path to STM32_Programmer_CLI if found on PATH, else None."""
    for name in PROGRAMMER_NAMES:
        path = shutil.which(name)
        if path:
            return path
    return None


def _build_flash_command(programmer: str, pkg_path: str,
                         app_start_addr: int,
                         connect_args: Optional[List[str]] = None) -> List[str]:
    if connect_args is None:
        connect_args = ['-c', 'port=SWD', 'freq=4000', 'reset=HWrst']
    return [
        programmer,
        *connect_args,
        '-d', str(pkg_path),
        '-s', f'0x{app_start_addr:08X}',
        '-v',  # verify after write
    ]


def dry_run_app(pkg_path: str,
                connect_args: Optional[List[str]] = None) -> Tuple[List[str], str]:
    """Validate the package and return the flash command without executing it.

    Returns:
        (command_list, human-readable status string)

    Raises:
        FileNotFoundError: package file does not exist.
        PackageValidationError: package is invalid.
    """
    if not Path(pkg_path).exists():
        raise FileNotFoundError(f"Package not found: {pkg_path}")

    hdr, _payload = parse_and_validate_package(pkg_path)
    programmer = detect_programmer() or '<STM32_Programmer_CLI>'
    cmd = _build_flash_command(programmer, pkg_path, hdr.app_start_addr, connect_args)

    status = (
        f"DRY-RUN: package valid\n"
        f"  firmware : v{hdr.fw_version[0]}.{hdr.fw_version[1]}.{hdr.fw_version[2]}\n"
        f"  app_size : {hdr.app_size} bytes\n"
        f"  app_addr : 0x{hdr.app_start_addr:08X}\n"
        f"  app_crc  : 0x{hdr.app_crc32:08X}\n"
        f"  programmer: {'found at ' + detect_programmer() if detect_programmer() else 'NOT FOUND on PATH'}\n"
        f"  command  : {' '.join(cmd)}"
    )
    return cmd, status


def execute_flash(pkg_path: str, confirm: bool = False,
                  connect_args: Optional[List[str]] = None) -> Tuple[int, str]:
    """Execute the flash command against real hardware.

    Requires confirm=True — this must be set by explicit user action (e.g. a
    checkbox in the GUI or --execute on the CLI).  Never call this from
    automated tests or background polling.

    Returns:
        (return_code, combined stdout+stderr output)

    Raises:
        RuntimeError: confirm is False.
        FileNotFoundError: programmer or package not found.
        PackageValidationError: package is invalid.
    """
    if not confirm:
        raise RuntimeError(
            "execute_flash() requires confirm=True.  Pass --execute on CLI "
            "or check the safety checkbox in the GUI.")

    programmer = detect_programmer()
    if not programmer:
        raise FileNotFoundError(
            "STM32_Programmer_CLI not found on PATH. "
            "Install STM32CubeProgrammer from st.com and ensure it is on PATH.")

    hdr, _payload = parse_and_validate_package(pkg_path)
    cmd    = _build_flash_command(programmer, pkg_path, hdr.app_start_addr, connect_args)
    result = subprocess.run(cmd, capture_output=True, text=True)
    return result.returncode, result.stdout + result.stderr
