"""test_backend.py — tests for the tool backend against FakeTargetInProcess.

Tests:
- capabilities_handshake → correct DeviceMode
- wrong hw_profile → UNSUPPORTED mode
- bootloader mode → BOOTLOADER mode
- poll_values / poll_cells / poll_temps / poll_faults / poll_diagnostics
- config read / validate / apply-RAM
- clear_latched_faults
- run_openwire
- wrong-target refusal (config ops blocked when UNSUPPORTED)
- package validate against target
- PollingLoop populates AppState
"""
import struct
import threading
import time

import pytest

from tool.src.fake_target.fake_target import FakeTarget, FakeTargetInProcess, TEMP_INVALID_CX10
from tool.src.protocol.framing import encode_frame
from tool.src.protocol.packet_defs import (
    FIRMWARE_TYPE_BOOTLOADER, TOTAL_CELL_COUNT, TOTAL_TEMP_COUNT,
)
from tool.src.core.connection_manager import TcpPort
from tool.src.core.target_model import TargetModel, TargetRefusedError
from tool.src.core.app_state import AppState
from tool.src.core.polling import PollingLoop
from tool.src.core.logging_model import EventLog
from tool.src.connection.device_state import DeviceMode
from tool.src.config.schema import BmsConfig
from tool.src.config.validator import validate_config


# ── In-process port adapter ───────────────────────────────────────────────────

class InProcessPort:
    """Connects TargetModel to a FakeTargetInProcess for unit testing without sockets."""

    def __init__(self, mode: str = 'healthy'):
        self._ft  = FakeTargetInProcess(mode=mode)
        self._buf = bytearray()

    def write(self, data: bytes) -> None:
        resp = self._ft.exchange(data)
        if resp:
            self._buf.extend(resp)

    def read(self, n: int) -> bytes:
        data = bytes(self._buf[:n])
        del self._buf[:n]
        return data

    @property
    def in_waiting(self) -> int:
        return len(self._buf)

    @property
    def ft(self) -> FakeTargetInProcess:
        return self._ft


def make_model(mode: str = 'healthy') -> tuple:
    port  = InProcessPort(mode)
    model = TargetModel(port)
    return model, port


# ── Capabilities handshake ────────────────────────────────────────────────────

class TestCapabilitiesHandshake:
    def test_healthy_mode_returns_bms_app(self):
        model, _ = make_model('healthy')
        device = model.capabilities_handshake()
        assert device.mode == DeviceMode.BMS_APP

    def test_bootloader_mode_returns_bootloader(self):
        model, _ = make_model('bootloader')
        device = model.capabilities_handshake()
        assert device.mode == DeviceMode.BOOTLOADER

    def test_wrong_hw_profile_returns_unsupported(self):
        # Patch FakeTarget to return wrong profile
        model, port = make_model('healthy')
        # Monkeypatch capabilities to return wrong profile
        orig_caps = port._ft.target._h_capabilities
        def bad_caps(seq):
            resp = bytearray(26)
            struct.pack_into('<H', resp, 0, 0x0001)
            struct.pack_into('<H', resp, 5, 0x9999)  # wrong profile
            struct.pack_into('<H', resp, 7, 1)
            struct.pack_into('<H', resp, 9, 1)
            resp[11] = 75; resp[12] = 75
            struct.pack_into('<I', resp, 13, 7)
            resp[17] = 9
            struct.pack_into('<I', resp, 18, 188*1024)
            struct.pack_into('<I', resp, 22, 8*1024)
            from tool.src.protocol.framing import encode_frame
            from tool.src.protocol.packet_defs import PKT_GET_CAPABILITIES
            return encode_frame(PKT_GET_CAPABILITIES, bytes(resp), seq=seq,
                                is_response=True)
        port._ft.target._h_capabilities = bad_caps
        device = model.capabilities_handshake()
        assert device.mode == DeviceMode.UNSUPPORTED
        assert 'hw_profile' in device.error_msg

    def test_capabilities_contain_cell_count(self):
        model, _ = make_model()
        model.capabilities_handshake()
        assert model.capabilities.cell_count == TOTAL_CELL_COUNT

    def test_capabilities_contain_temp_count(self):
        model, _ = make_model()
        model.capabilities_handshake()
        assert model.capabilities.temp_count == TOTAL_TEMP_COUNT


# ── Polling methods ───────────────────────────────────────────────────────────

class TestPolling:
    def setup_method(self):
        self.model, self.port = make_model('healthy')
        self.model.capabilities_handshake()

    def test_poll_values_returns_valid(self):
        vs = self.model.poll_values()
        assert vs.valid

    def test_poll_cells_returns_75_cells(self):
        cs = self.model.poll_cells()
        assert cs.cell_count == TOTAL_CELL_COUNT
        assert len(cs.cells_mv) == TOTAL_CELL_COUNT
        assert cs.valid

    def test_poll_cells_nominal_voltage(self):
        cs = self.model.poll_cells()
        assert all(mv == 3700 for mv in cs.cells_mv)

    def test_poll_temps_returns_75_temps(self):
        ts = self.model.poll_temps()
        assert ts.temp_count == TOTAL_TEMP_COUNT
        assert ts.valid

    def test_poll_faults_no_faults_in_healthy(self):
        fs = self.model.poll_faults()
        assert fs.active_faults == 0
        assert fs.latched_faults == 0
        assert fs.valid

    def test_poll_diagnostics_valid(self):
        ds = self.model.poll_diagnostics()
        assert ds.valid
        assert ds.reset_cause == 0x01  # POR
        assert ds.pec_cell_errors == 0

    def test_cell_uv_mode_sets_fault(self):
        model, _ = make_model('cell_uv')
        model.capabilities_handshake()
        fs = model.poll_faults()
        assert fs.active_faults & (1 << 1)  # FAULT_CELL_UV

    def test_cell_uv_mode_cell_voltage_is_2400(self):
        model, _ = make_model('cell_uv')
        model.capabilities_handshake()
        cs = model.poll_cells()
        assert cs.cells_mv[0] == 2400

    def test_poll_requires_app_mode(self):
        model, _ = make_model('bootloader')
        model.capabilities_handshake()
        with pytest.raises(TargetRefusedError):
            model.poll_values()

    def test_poll_without_handshake_raises(self):
        model, _ = make_model()
        # No handshake — mode is DISCONNECTED
        with pytest.raises(TargetRefusedError):
            model.poll_values()


# ── Config operations ─────────────────────────────────────────────────────────

class TestConfigOps:
    def setup_method(self):
        self.model, self.port = make_model('healthy')
        self.model.capabilities_handshake()

    def test_read_config_returns_bms_config(self):
        cfg = self.model.read_config()
        assert isinstance(cfg, BmsConfig)
        assert cfg.cell_count == 75

    def test_validate_config_offline_default_passes(self):
        cfg = BmsConfig()
        ok, err_off, msg = self.model.validate_config_offline(cfg)
        assert ok, msg

    def test_validate_config_on_target_default_passes(self):
        cfg = BmsConfig()
        ok, err_off, msg = self.model.validate_config_on_target(cfg)
        assert ok, msg

    def test_apply_config_ram_default_passes(self):
        cfg = BmsConfig()
        ok, err_off, msg = self.model.apply_config_ram(cfg)
        assert ok, msg

    def test_apply_invalid_config_fails(self):
        cfg = BmsConfig()
        cfg.cell_uv_hard_mv = 5000  # > cell_uv_soft_mv — invalid
        ok, err_off, msg = self.model.apply_config_ram(cfg)
        assert not ok

    def test_config_ops_blocked_in_bootloader(self):
        model, _ = make_model('bootloader')
        model.capabilities_handshake()
        with pytest.raises(TargetRefusedError):
            model.read_config()
        with pytest.raises(TargetRefusedError):
            model.apply_config_ram(BmsConfig())


# ── Fault clearing ────────────────────────────────────────────────────────────

class TestFaultClearing:
    def setup_method(self):
        self.model, self.port = make_model('healthy')
        self.model.capabilities_handshake()

    def test_inject_then_clear_latched(self):
        self.port.ft.inject_fault(0)  # FAULT_CELL_OV
        fs = self.model.poll_faults()
        assert fs.active_faults & 1
        assert fs.latched_faults & 1

        # Active still set — clear should return 0 cleared
        cleared = self.model.clear_latched_faults(0xFFFFFFFFFFFFFFFF)
        assert (cleared & 1) == 0  # not cleared (active still set)

        # Clear active then try again
        self.port.ft.clear_fault(0)
        cleared = self.model.clear_latched_faults(0xFFFFFFFFFFFFFFFF)
        assert cleared & 1  # now cleared

    def test_clear_latched_faults_requires_app_mode(self):
        model, _ = make_model('bootloader')
        model.capabilities_handshake()
        with pytest.raises(TargetRefusedError):
            model.clear_latched_faults(0xFFFF)


# ── Openwire ──────────────────────────────────────────────────────────────────

class TestRunOpenwire:
    def test_run_openwire_succeeds(self):
        model, _ = make_model('healthy')
        model.capabilities_handshake()
        result = model.run_openwire()
        assert result['status'] == 0
        assert len(result['open_wire_mask']) == 10

    def test_openwire_diagnostic_updates_diagnostics(self):
        model, _ = make_model('healthy')
        model.capabilities_handshake()
        model.run_openwire()
        ds = model.poll_diagnostics()
        assert ds.open_wire_valid


# ── Package validation against target ────────────────────────────────────────

class TestPackageValidation:
    def test_valid_package_accepted(self):
        from tool.src.update.package_builder import build_package
        from tool.src.update.package_parser import parse_header, PKG_HEADER_SIZE
        fw  = bytes(range(256)) * 4
        pkg = build_package(fw, fw_version=(0, 1, 0))
        hdr = parse_header(pkg[:PKG_HEADER_SIZE])
        model, _ = make_model()
        model.capabilities_handshake()
        ok, msg = model.validate_package_against_target(hdr)
        assert ok, msg

    def test_wrong_profile_package_rejected(self):
        from tool.src.update.package_parser import PackageHeader
        bad_hdr = PackageHeader(
            pkg_magic=0xBF00BF00, pkg_version=1, hw_profile_id=0x9999,
            target_mcu_id=0x422, image_type=1,
            app_start_addr=0x08008000, app_size=1024, app_crc32=0,
            fw_version=(0, 1, 0), min_bootloader_version=(0, 1, 0),
            required_config_schema=1, pkg_header_crc32=0,
        )
        model, _ = make_model()
        model.capabilities_handshake()
        ok, msg = model.validate_package_against_target(bad_hdr)
        assert not ok
        assert 'hw_profile' in msg


# ── PollingLoop + AppState ────────────────────────────────────────────────────

class TestPollingLoop:
    def test_polling_loop_populates_app_state(self):
        model, _ = make_model('healthy')
        model.capabilities_handshake()

        state  = AppState()
        evt_log = EventLog()
        loop   = PollingLoop(model, state, evt_log, interval=0.05)
        loop.start()
        time.sleep(0.3)
        loop.stop()

        assert state.values.valid
        assert state.cells.valid
        assert state.temps.valid
        assert state.faults.valid

    def test_polling_loop_updates_fault_state(self):
        model, port = make_model('healthy')
        model.capabilities_handshake()
        port.ft.inject_fault(0)  # FAULT_CELL_OV

        state = AppState()
        loop  = PollingLoop(model, state, interval=0.05)
        loop.start()
        time.sleep(0.3)
        loop.stop()

        assert state.faults.active_faults & 1

    def test_polling_loop_stop_is_clean(self):
        model, _ = make_model()
        model.capabilities_handshake()
        state = AppState()
        loop  = PollingLoop(model, state, interval=0.05)
        loop.start()
        assert loop.running
        loop.stop(timeout=1.0)
        assert not loop.running


# ── AppState observer ─────────────────────────────────────────────────────────

class TestAppState:
    def test_subscribe_receives_notifications(self):
        state    = AppState()
        received = []
        state.subscribe(lambda key: received.append(key))

        from tool.src.core.app_state import ValuesState
        state.update_values(ValuesState(valid=True))
        assert 'values' in received

    def test_reset_fires_reset_event(self):
        state    = AppState()
        received = []
        state.subscribe(lambda key: received.append(key))
        state.reset()
        assert 'reset' in received

    def test_unsubscribe_stops_notifications(self):
        state    = AppState()
        received = []
        fn = lambda key: received.append(key)
        state.subscribe(fn)
        state.unsubscribe(fn)
        from tool.src.core.app_state import ValuesState
        state.update_values(ValuesState(valid=True))
        assert not received
