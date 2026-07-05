"""package_parser.py — firmware package header parse and validate.

Mirrors bl_validate.c validation order (docs/06_flash_and_bootloader.md).
"""
import struct
from dataclasses import dataclass
from ..protocol.crc import crc32_iso_hdlc
from ..protocol.packet_defs import HW_PROFILE_ID

PKG_MAGIC         = 0xBF00BF00
PKG_HEADER_SIZE   = 64
BL_MAX_PKG_VERSION = 1
APP_START_ADDR    = 0x08008000
APP_REGION_SIZE   = 188 * 1024
# Last 2 KB page of the app region stores the bootloader's persisted package
# header (metadata) — usable image size is one page smaller.
APP_MAX_SIZE      = APP_REGION_SIZE - 2048
STM32F303VC_DEV_ID = 0x422


# Header format: 64 bytes, packed LE
# Offsets match bl_validate.h FirmwarePackageHeader
_HDR_FMT = '<IHHIB3xIIIBBBBBBHHI22x'
_HDR_FIELDS = (
    'pkg_magic', 'pkg_version', 'hw_profile_id', 'target_mcu_id',
    'image_type',
    'app_start_addr', 'app_size', 'app_crc32',
    'fw_ver_major', 'fw_ver_minor', 'fw_ver_patch',
    'min_bl_major', 'min_bl_minor', 'min_bl_patch',
    'required_config_schema', '_reserved',
    'pkg_header_crc32',
)
assert struct.calcsize(_HDR_FMT) == PKG_HEADER_SIZE, \
    f"Header struct size {struct.calcsize(_HDR_FMT)} != {PKG_HEADER_SIZE}"


@dataclass
class PackageHeader:
    pkg_magic:              int
    pkg_version:            int
    hw_profile_id:          int
    target_mcu_id:          int
    image_type:             int
    app_start_addr:         int
    app_size:               int
    app_crc32:              int
    fw_version:             tuple   # (major, minor, patch)
    min_bootloader_version: tuple   # (major, minor, patch)
    required_config_schema: int
    pkg_header_crc32:       int


class PackageValidationError(Exception):
    def __init__(self, reason: str):
        self.reason = reason
        super().__init__(reason)


def parse_header(data: bytes) -> PackageHeader:
    """Parse the 64-byte package header."""
    if len(data) < PKG_HEADER_SIZE:
        raise PackageValidationError(f"Header too short: {len(data)} bytes")
    fields = struct.unpack_from(_HDR_FMT, data)
    return PackageHeader(
        pkg_magic=fields[0], pkg_version=fields[1], hw_profile_id=fields[2],
        target_mcu_id=fields[3], image_type=fields[4],
        app_start_addr=fields[5], app_size=fields[6], app_crc32=fields[7],
        fw_version=(fields[8], fields[9], fields[10]),
        min_bootloader_version=(fields[11], fields[12], fields[13]),
        required_config_schema=fields[14],
        pkg_header_crc32=fields[16],
    )


def validate_header(hdr: PackageHeader,
                    mcu_dev_id: int = STM32F303VC_DEV_ID,
                    raw_header: bytes = None) -> None:
    """Validate a parsed header. Raises PackageValidationError on failure.
    raw_header must be the original 64-byte blob (needed for CRC check).
    """
    if hdr.pkg_magic != PKG_MAGIC:
        raise PackageValidationError(f"Bad magic: 0x{hdr.pkg_magic:08X}")
    if hdr.pkg_version == 0 or hdr.pkg_version > BL_MAX_PKG_VERSION:
        raise PackageValidationError(f"Unsupported pkg_version: {hdr.pkg_version}")
    if hdr.hw_profile_id != HW_PROFILE_ID:
        raise PackageValidationError(
            f"Wrong hw_profile_id: 0x{hdr.hw_profile_id:04X} (expected 0x{HW_PROFILE_ID:04X})")
    if (hdr.target_mcu_id & 0xFFF) != (mcu_dev_id & 0xFFF):
        raise PackageValidationError(
            f"Wrong MCU dev_id: 0x{hdr.target_mcu_id & 0xFFF:03X} "
            f"(expected 0x{mcu_dev_id & 0xFFF:03X})")
    if hdr.image_type != 0x01:
        raise PackageValidationError(f"Wrong image_type: 0x{hdr.image_type:02X} (must be 0x01)")
    if hdr.app_start_addr != APP_START_ADDR:
        raise PackageValidationError(
            f"Wrong app_start_addr: 0x{hdr.app_start_addr:08X}")
    if hdr.app_size == 0 or hdr.app_size > APP_MAX_SIZE:
        raise PackageValidationError(
            f"Invalid app_size: {hdr.app_size} (max {APP_MAX_SIZE})")
    if raw_header is not None:
        # CRC covers bytes [0x00..0x25] (38 bytes) with crc field at 0x26..0x29 excluded
        computed = crc32_iso_hdlc(raw_header[:0x26])
        if computed != hdr.pkg_header_crc32:
            raise PackageValidationError(
                f"Header CRC mismatch: got 0x{hdr.pkg_header_crc32:08X}, "
                f"computed 0x{computed:08X}")


def parse_and_validate_package(path: str,
                                mcu_dev_id: int = STM32F303VC_DEV_ID) -> tuple:
    """Parse and validate a .pkg file. Returns (header, payload_bytes).
    Raises PackageValidationError on any failure.
    """
    with open(path, 'rb') as f:
        raw = f.read()
    if len(raw) < PKG_HEADER_SIZE:
        raise PackageValidationError("File smaller than package header")
    raw_header = raw[:PKG_HEADER_SIZE]
    hdr = parse_header(raw_header)
    validate_header(hdr, mcu_dev_id=mcu_dev_id, raw_header=raw_header)
    payload = raw[PKG_HEADER_SIZE:]
    if len(payload) != hdr.app_size:
        raise PackageValidationError(
            f"Payload size mismatch: file has {len(payload)}, header says {hdr.app_size}")
    # Verify payload CRC
    payload_crc = crc32_iso_hdlc(payload)
    if payload_crc != hdr.app_crc32:
        raise PackageValidationError(
            f"Payload CRC mismatch: got 0x{hdr.app_crc32:08X}, computed 0x{payload_crc:08X}")
    return hdr, payload
