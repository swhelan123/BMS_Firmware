"""package_builder.py — build a BMS firmware .pkg file from a binary image.

A .pkg file = 64-byte header + raw firmware binary payload.
Header format mirrors bl_validate.h FirmwarePackageHeader.

Usage:
    from tool.src.update.package_builder import build_package
    pkg_bytes = build_package(
        firmware_binary=open('firmware.bin','rb').read(),
        fw_version=(0, 1, 0),
    )
    open('firmware.pkg', 'wb').write(pkg_bytes)

Or as CLI:
    python -m tool.src.update.package_builder firmware.bin firmware.pkg --version 0.1.0
"""
import struct
from typing import Tuple

from ..protocol.crc import crc32_iso_hdlc
from .package_parser import (
    PKG_MAGIC, PKG_HEADER_SIZE, BL_MAX_PKG_VERSION,
    APP_START_ADDR, APP_MAX_SIZE, STM32F303VC_DEV_ID, _HDR_FMT,
)
from ..protocol.packet_defs import HW_PROFILE_ID

# image_type = 0x01 means application firmware
IMAGE_TYPE_APP = 0x01

# Minimum bootloader version required to accept this package format
MIN_BL_VERSION = (0, 1, 0)

# Config schema version required by this firmware
REQUIRED_CONFIG_SCHEMA = 1


class PackageBuildError(Exception):
    pass


def build_package(
    firmware_binary: bytes,
    fw_version: Tuple[int, int, int] = (0, 1, 0),
    hw_profile_id: int = HW_PROFILE_ID,
    target_mcu_id: int = STM32F303VC_DEV_ID,
    image_type: int = IMAGE_TYPE_APP,
    app_start_addr: int = APP_START_ADDR,
    min_bl_version: Tuple[int, int, int] = MIN_BL_VERSION,
    required_config_schema: int = REQUIRED_CONFIG_SCHEMA,
) -> bytes:
    """Build a .pkg binary from a raw firmware image.

    Args:
        firmware_binary: Raw .bin contents of the firmware image.
        fw_version: (major, minor, patch) version tuple.
        All other args override defaults for non-standard builds.

    Returns:
        Complete .pkg binary (header + payload).

    Raises:
        PackageBuildError: If the firmware is too large or version fields overflow.
    """
    app_size = len(firmware_binary)
    if app_size == 0:
        raise PackageBuildError("Firmware binary is empty")
    if app_size > APP_MAX_SIZE:
        raise PackageBuildError(
            f"Firmware too large: {app_size} bytes > {APP_MAX_SIZE} max")

    for name, val in [('fw_version', fw_version), ('min_bl_version', min_bl_version)]:
        for component in val:
            if not (0 <= component <= 255):
                raise PackageBuildError(f"{name} component {component} out of [0, 255]")

    # Compute payload CRC
    app_crc32 = crc32_iso_hdlc(firmware_binary)

    # Build header with pkg_header_crc32 = 0 initially
    header_no_crc = struct.pack(
        _HDR_FMT,
        PKG_MAGIC,
        BL_MAX_PKG_VERSION,   # pkg_version
        hw_profile_id,
        target_mcu_id,
        image_type,
        app_start_addr,
        app_size,
        app_crc32,
        fw_version[0], fw_version[1], fw_version[2],
        min_bl_version[0], min_bl_version[1], min_bl_version[2],
        required_config_schema,
        0,                    # _reserved
        0,                    # pkg_header_crc32 placeholder
    )
    assert len(header_no_crc) == PKG_HEADER_SIZE, \
        f"Header size {len(header_no_crc)} != {PKG_HEADER_SIZE}"

    # CRC covers bytes [0x00..0x25] (38 bytes); pkg_header_crc32 is at offset 0x26
    hdr_crc = crc32_iso_hdlc(header_no_crc[:0x26])

    # Patch the CRC field at offset 0x26
    header = bytearray(header_no_crc)
    struct.pack_into('<I', header, 0x26, hdr_crc)

    return bytes(header) + firmware_binary


if __name__ == '__main__':
    import argparse
    import sys

    parser = argparse.ArgumentParser(description='Build BMS firmware .pkg file')
    parser.add_argument('input',  help='Input firmware .bin')
    parser.add_argument('output', help='Output .pkg file path')
    parser.add_argument('--version', default='0.1.0',
                        help='Firmware version, e.g. 0.1.0')
    args = parser.parse_args()

    try:
        major, minor, patch = (int(x) for x in args.version.split('.'))
    except ValueError:
        print(f"Invalid version: {args.version}", file=sys.stderr)
        sys.exit(1)

    with open(args.input, 'rb') as f:
        binary = f.read()

    try:
        pkg = build_package(binary, fw_version=(major, minor, patch))
    except PackageBuildError as e:
        print(f"Error: {e}", file=sys.stderr)
        sys.exit(1)

    with open(args.output, 'wb') as f:
        f.write(pkg)

    print(f"Written {len(pkg)} bytes to {args.output} "
          f"(header: {PKG_HEADER_SIZE}, payload: {len(binary)})")
