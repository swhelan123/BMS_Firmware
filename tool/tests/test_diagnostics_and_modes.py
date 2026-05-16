"""test_diagnostics_and_modes.py — tests for diagnostics handlers, simulation modes,
package builder, and FakeTargetInProcess.
"""
import struct
import pytest
import sys, os
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', '..'))

from tool.src.fake_target.fake_target import FakeTarget, FakeTargetInProcess
from tool.src.protocol.framing import encode_frame, decode_frame
from tool.src.protocol.packet_defs import (
    PKT_GET_DIAGNOSTICS_SUMMARY, PKT_RUN_OPENWIRE,
    PKT_GET_BOOT_INFO, PKT_BOOT_UPDATE_BEGIN, PKT_BOOT_UPDATE_CHUNK,
    PKT_BOOT_UPDATE_FINALIZE, PKT_BOOT_UPDATE_ABORT,
    TOTAL_CELL_COUNT, FIRMWARE_TYPE_BMS_APP,
)
from tool.src.update.package_builder import build_package, PackageBuildError, PKG_HEADER_SIZE
from tool.src.update.package_parser import (
    parse_and_validate_package, parse_header, validate_header,
    PackageValidationError,
)
from tool.src.protocol.crc import crc32_iso_hdlc


# ── Helpers ────────────────────────────────────────────────────────────────────

def send_recv(target: FakeTarget, pkt_id: int, payload: bytes = b'') -> dict:
    frame = encode_frame(pkt_id, payload, seq=1)
    resp_bytes = target.feed(frame)
    assert resp_bytes, f"No response for 0x{pkt_id:04X}"
    return decode_frame(resp_bytes)


# ── Diagnostics summary handler ────────────────────────────────────────────────

class TestDiagnosticsSummary:
    def test_response_length_is_28(self):
        t = FakeTarget()
        r = send_recv(t, PKT_GET_DIAGNOSTICS_SUMMARY)
        assert len(r['payload']) == 28

    def test_default_reset_cause_is_por(self):
        t = FakeTarget()
        r = send_recv(t, PKT_GET_DIAGNOSTICS_SUMMARY)
        reset_cause = r['payload'][0]
        assert reset_cause == 0x01  # RESET_CAUSE_POR

    def test_default_pec_errors_are_zero(self):
        t = FakeTarget()
        r = send_recv(t, PKT_GET_DIAGNOSTICS_SUMMARY)
        pec_cell = struct.unpack_from('<I', r['payload'], 1)[0]
        pec_temp = struct.unpack_from('<I', r['payload'], 5)[0]
        assert pec_cell == 0
        assert pec_temp == 0

    def test_open_wire_not_valid_by_default(self):
        t = FakeTarget()
        r = send_recv(t, PKT_GET_DIAGNOSTICS_SUMMARY)
        open_wire_valid = r['payload'][13]
        assert open_wire_valid == 0

    def test_pec_errors_reflected_after_set(self):
        t = FakeTarget()
        t.set_pec_errors(cell=42, temp=7)
        r = send_recv(t, PKT_GET_DIAGNOSTICS_SUMMARY)
        pec_cell = struct.unpack_from('<I', r['payload'], 1)[0]
        pec_temp = struct.unpack_from('<I', r['payload'], 5)[0]
        assert pec_cell == 42
        assert pec_temp == 7

    def test_response_is_not_error(self):
        t = FakeTarget()
        r = send_recv(t, PKT_GET_DIAGNOSTICS_SUMMARY)
        assert r['is_response']
        assert not r['is_error']


# ── Run open-wire handler ──────────────────────────────────────────────────────

class TestRunOpenwire:
    def test_response_length_is_11(self):
        t = FakeTarget()
        r = send_recv(t, PKT_RUN_OPENWIRE)
        assert len(r['payload']) == 11

    def test_status_byte_is_zero_on_success(self):
        t = FakeTarget()
        r = send_recv(t, PKT_RUN_OPENWIRE)
        assert r['payload'][0] == 0  # success

    def test_no_open_wires_in_healthy_mode(self):
        t = FakeTarget()
        r = send_recv(t, PKT_RUN_OPENWIRE)
        # Bytes [1:11] are the open-wire bitmask — should be all zeros
        assert all(b == 0 for b in r['payload'][1:])

    def test_openwire_valid_set_after_run(self):
        t = FakeTarget()
        send_recv(t, PKT_RUN_OPENWIRE)
        # Subsequent diagnostics summary should show open_wire_valid = 1
        r = send_recv(t, PKT_GET_DIAGNOSTICS_SUMMARY)
        assert r['payload'][13] == 1


# ── Boot info handler ──────────────────────────────────────────────────────────

class TestGetBootInfo:
    def test_response_length_is_14(self):
        t = FakeTarget()
        r = send_recv(t, PKT_GET_BOOT_INFO)
        assert len(r['payload']) == 14

    def test_firmware_type_is_bms_app(self):
        t = FakeTarget()
        r = send_recv(t, PKT_GET_BOOT_INFO)
        fw_type = struct.unpack_from('<H', r['payload'], 0)[0]
        assert fw_type == FIRMWARE_TYPE_BMS_APP

    def test_reset_cause_is_por(self):
        t = FakeTarget()
        r = send_recv(t, PKT_GET_BOOT_INFO)
        reset_cause = r['payload'][9]
        assert reset_cause == 0x01


# ── Bootloader update packets blocked outside bootloader mode ─────────────────

class TestBootloaderUpdateBlockedInAppMode:
    @pytest.mark.parametrize("pkt_id", [
        PKT_BOOT_UPDATE_BEGIN,
        PKT_BOOT_UPDATE_CHUNK,
        PKT_BOOT_UPDATE_FINALIZE,
    ])
    def test_returns_error_in_bms_app_mode(self, pkt_id):
        """BEGIN / CHUNK / FINALIZE must error when not in BOOTLOADER mode."""
        t = FakeTarget()   # defaults to healthy / BMS_APP
        r = send_recv(t, pkt_id)
        assert r['is_error']

    def test_abort_always_succeeds(self):
        """ABORT is idempotent and succeeds in any mode (safe to call anytime)."""
        t = FakeTarget()
        r = send_recv(t, PKT_BOOT_UPDATE_ABORT)
        assert not r['is_error']


# ── Simulation modes ──────────────────────────────────────────────────────────

FAULT_BIT_CELL_OV           = 0
FAULT_BIT_CELL_UV           = 1
FAULT_BIT_TEMP_READ_INVALID = 9
FAULT_BIT_ISOSPI_CELL       = 15
FAULT_BIT_CONFIG_INVALID    = 19


class TestSimulationModes:
    def test_healthy_mode_no_faults(self):
        t = FakeTarget(mode='healthy')
        r = send_recv(t, PKT_GET_BOOT_INFO)  # just any request
        assert not r['is_error']
        # Check faults are zero
        from tool.src.protocol.packet_defs import PKT_GET_FAULTS
        rf = send_recv(t, PKT_GET_FAULTS)
        active = struct.unpack_from('<Q', rf['payload'], 0)[0]
        assert active == 0

    def test_cell_uv_mode_sets_uv_fault(self):
        t = FakeTarget(mode='cell_uv')
        from tool.src.protocol.packet_defs import PKT_GET_FAULTS
        r = send_recv(t, PKT_GET_FAULTS)
        active = struct.unpack_from('<Q', r['payload'], 0)[0]
        assert active & (1 << FAULT_BIT_CELL_UV)

    def test_cell_uv_mode_has_low_voltage_cell(self):
        t = FakeTarget(mode='cell_uv')
        from tool.src.protocol.packet_defs import PKT_GET_CELLS
        r = send_recv(t, PKT_GET_CELLS, b'\x00')
        mv0 = struct.unpack_from('<H', r['payload'], 2)[0]
        assert mv0 == 2400  # the under-voltage cell

    def test_cell_ov_mode_sets_ov_fault(self):
        t = FakeTarget(mode='cell_ov')
        from tool.src.protocol.packet_defs import PKT_GET_FAULTS
        r = send_recv(t, PKT_GET_FAULTS)
        active = struct.unpack_from('<Q', r['payload'], 0)[0]
        assert active & (1 << FAULT_BIT_CELL_OV)

    def test_temp_invalid_mode_sets_temp_fault(self):
        t = FakeTarget(mode='temp_invalid')
        from tool.src.protocol.packet_defs import PKT_GET_FAULTS
        r = send_recv(t, PKT_GET_FAULTS)
        active = struct.unpack_from('<Q', r['payload'], 0)[0]
        assert active & (1 << FAULT_BIT_TEMP_READ_INVALID)

    def test_isospi_fault_mode_sets_isospi_cell_fault(self):
        t = FakeTarget(mode='isospi_fault')
        from tool.src.protocol.packet_defs import PKT_GET_FAULTS
        r = send_recv(t, PKT_GET_FAULTS)
        active = struct.unpack_from('<Q', r['payload'], 0)[0]
        assert active & (1 << FAULT_BIT_ISOSPI_CELL)

    def test_config_error_mode_sets_config_fault(self):
        t = FakeTarget(mode='config_error')
        from tool.src.protocol.packet_defs import PKT_GET_FAULTS
        r = send_recv(t, PKT_GET_FAULTS)
        active = struct.unpack_from('<Q', r['payload'], 0)[0]
        assert active & (1 << FAULT_BIT_CONFIG_INVALID)

    def test_unknown_mode_raises(self):
        with pytest.raises(ValueError, match="Unknown simulation mode"):
            FakeTarget(mode='nonexistent_mode')


# ── FakeTargetInProcess ────────────────────────────────────────────────────────

class TestFakeTargetInProcess:
    def test_exchange_returns_response(self):
        ft = FakeTargetInProcess(mode='healthy')
        from tool.src.protocol.packet_defs import PKT_GET_CAPABILITIES
        frame = encode_frame(PKT_GET_CAPABILITIES, b'', seq=1)
        resp = ft.exchange(frame)
        assert resp
        d = decode_frame(resp)
        assert not d['is_error']

    def test_inject_fault_reflected(self):
        ft = FakeTargetInProcess()
        ft.inject_fault(FAULT_BIT_CELL_OV)
        from tool.src.protocol.packet_defs import PKT_GET_FAULTS
        frame = encode_frame(PKT_GET_FAULTS, b'', seq=1)
        resp = ft.exchange(frame)
        d = decode_frame(resp)
        active = struct.unpack_from('<Q', d['payload'], 0)[0]
        assert active & (1 << FAULT_BIT_CELL_OV)

    def test_cell_mv_set_reflected(self):
        ft = FakeTargetInProcess()
        ft.set_cell_mv([3900] * TOTAL_CELL_COUNT)
        from tool.src.protocol.packet_defs import PKT_GET_CELLS
        frame = encode_frame(PKT_GET_CELLS, b'\x00', seq=1)
        resp = ft.exchange(frame)
        d = decode_frame(resp)
        mv0 = struct.unpack_from('<H', d['payload'], 2)[0]
        assert mv0 == 3900


# ── Package builder ────────────────────────────────────────────────────────────

class TestPackageBuilder:
    def _minimal_firmware(self, size: int = 1024) -> bytes:
        return bytes(range(256)) * (size // 256) + bytes(range(size % 256))

    def test_build_produces_correct_total_size(self):
        fw = self._minimal_firmware(1024)
        pkg = build_package(fw, fw_version=(0, 1, 0))
        assert len(pkg) == PKG_HEADER_SIZE + 1024

    def test_built_package_parses_successfully(self, tmp_path):
        fw = self._minimal_firmware(2048)
        pkg = build_package(fw, fw_version=(0, 2, 3))
        pkg_file = tmp_path / "firmware.pkg"
        pkg_file.write_bytes(pkg)
        hdr, payload = parse_and_validate_package(str(pkg_file))
        assert hdr.fw_version == (0, 2, 3)
        assert payload == fw

    def test_built_package_has_correct_payload_crc(self):
        fw = self._minimal_firmware(512)
        pkg = build_package(fw)
        hdr = parse_header(pkg[:PKG_HEADER_SIZE])
        assert hdr.app_crc32 == crc32_iso_hdlc(fw)

    def test_empty_firmware_raises(self):
        with pytest.raises(PackageBuildError, match="empty"):
            build_package(b'')

    def test_oversized_firmware_raises(self):
        from tool.src.update.package_builder import APP_REGION_SIZE
        too_big = bytes(APP_REGION_SIZE + 1)
        with pytest.raises(PackageBuildError, match="too large"):
            build_package(too_big)

    def test_firmware_version_stored_correctly(self):
        fw = self._minimal_firmware(256)
        pkg = build_package(fw, fw_version=(1, 2, 3))
        hdr = parse_header(pkg[:PKG_HEADER_SIZE])
        assert hdr.fw_version == (1, 2, 3)

    def test_header_crc_is_valid(self):
        fw = self._minimal_firmware(256)
        pkg = build_package(fw)
        hdr = parse_header(pkg[:PKG_HEADER_SIZE])
        expected_crc = crc32_iso_hdlc(pkg[:0x26])
        assert hdr.pkg_header_crc32 == expected_crc

    def test_round_trip_parse_validate(self, tmp_path):
        fw = bytes(range(256)) * 8  # 2048 bytes
        pkg = build_package(fw, fw_version=(0, 1, 0))
        path = tmp_path / "fw.pkg"
        path.write_bytes(pkg)
        hdr, recovered = parse_and_validate_package(str(path))
        assert recovered == fw
        assert not hdr.pkg_header_crc32 == 0
