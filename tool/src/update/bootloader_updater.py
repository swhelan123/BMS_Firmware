"""bootloader_updater.py — firmware update via the bootloader protocol.

Usage:
    updater = BootloaderUpdater(client)
    result  = updater.update('firmware.pkg', on_progress=lambda done, total: ...)
    if not result.success:
        raise RuntimeError(result.message)
"""
import struct
from dataclasses import dataclass, field
from typing import Callable, Optional

from ..protocol.client import BmsProtocolClient, ProtocolError
from ..update.package_parser import (
    parse_and_validate_package, PackageValidationError, PKG_HEADER_SIZE,
)


@dataclass
class UpdateResult:
    success:      bool
    message:      str
    chunks_sent:  int = 0
    total_chunks: int = 0
    computed_crc: int = 0


class UpdateError(Exception):
    pass


class BootloaderUpdater:
    """Runs the full BEGIN → CHUNK* → FINALIZE bootloader update sequence."""

    def __init__(self, client: BmsProtocolClient):
        self._client = client

    def update(self, pkg_path: str,
               on_progress: Optional[Callable[[int, int], None]] = None) -> UpdateResult:
        """Flash a .pkg file through the bootloader protocol.

        Args:
            pkg_path:    Path to a validated .pkg firmware package.
            on_progress: Optional callback(chunks_done, total_chunks) per chunk.

        Returns:
            UpdateResult with success=True and CRC on success.

        Raises:
            UpdateError: On any protocol failure or package problem.
        """
        try:
            hdr, payload = parse_and_validate_package(pkg_path)
        except PackageValidationError as e:
            raise UpdateError(f"Package invalid: {e}") from e
        except FileNotFoundError:
            raise UpdateError(f"Package not found: {pkg_path}")

        with open(pkg_path, 'rb') as f:
            raw_hdr_bytes = f.read(PKG_HEADER_SIZE)

        # BEGIN
        try:
            begin = self._client.boot_update_begin(raw_hdr_bytes)
        except ProtocolError as e:
            raise UpdateError(f"BEGIN failed: {e}") from e

        if begin['result'] != 0:
            reason = begin['reject_reason']
            raise UpdateError(f"BEGIN rejected by bootloader (reason=0x{reason:02X})")

        chunk_size   = begin['expected_chunk_size']
        total_chunks = begin['total_chunks']

        # CHUNK
        sent   = 0
        offset = 0
        while offset < len(payload):
            chunk = payload[offset:offset + chunk_size]
            try:
                result = self._client.boot_update_chunk(sent, chunk)
            except ProtocolError as e:
                self._abort()
                raise UpdateError(f"CHUNK {sent}/{total_chunks} failed: {e}") from e

            if result != 0:
                self._abort()
                raise UpdateError(
                    f"CHUNK {sent}/{total_chunks} rejected (0x{result:02X})")

            sent   += 1
            offset += len(chunk)
            if on_progress:
                on_progress(sent, total_chunks)

        # FINALIZE
        try:
            fin = self._client.boot_update_finalize()
        except ProtocolError as e:
            raise UpdateError(f"FINALIZE failed: {e}") from e

        if fin['result'] != 0:
            raise UpdateError(f"FINALIZE rejected (result=0x{fin['result']:02X})")

        return UpdateResult(
            success=True,
            message="Update complete",
            chunks_sent=sent,
            total_chunks=total_chunks,
            computed_crc=fin['computed_crc'],
        )

    def _abort(self) -> None:
        try:
            self._client.boot_update_abort()
        except ProtocolError:
            pass
