"""test_fake_target.py — integration tests against the in-process fake target."""
import struct
import pytest
import sys, os
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', '..'))

from tool.src.fake_target.fake_target import FakeTarget
from tool.src.protocol.framing import encode_frame, decode_frame
from tool.src.protocol.packet_defs import (
    PKT_GET_CAPABILITIES, PKT_GET_VALUES, PKT_GET_CELLS, PKT_GET_TEMPS,
    PKT_GET_FAULTS, PKT_CLEAR_LATCHED_FAULTS, PKT_GET_CONFIG,
    PKT_VALIDATE_CONFIG, PKT_SET_CONFIG_RAM, PKT_STORE_CONFIG,
    FIRMWARE_TYPE_BMS_APP, TOTAL_CELL_COUNT, TOTAL_TEMP_COUNT,
)
from tool.src.config.schema import BmsConfig


def send_recv(target: FakeTarget, pkt_id: int, payload: bytes = b'') -> dict:
    frame = encode_frame(pkt_id, payload, seq=1)
    resp_bytes = target.feed(frame)
    assert resp_bytes, f"No response for pkt_id=0x{pkt_id:04X}"
    return decode_frame(resp_bytes)


class TestCapabilities:
    def test_capabilities_returns_bms_app_type(self):
        t = FakeTarget()
        r = send_recv(t, PKT_GET_CAPABILITIES)
        ft = struct.unpack_from('<H', r['payload'], 0)[0]
        assert ft == FIRMWARE_TYPE_BMS_APP

    def test_capabilities_correct_cell_count(self):
        t = FakeTarget()
        r = send_recv(t, PKT_GET_CAPABILITIES)
        assert r['payload'][11] == TOTAL_CELL_COUNT

    def test_capabilities_is_response(self):
        t = FakeTarget()
        r = send_recv(t, PKT_GET_CAPABILITIES)
        assert r['is_response']
        assert not r['is_error']


class TestGetCells:
    def test_returns_75_cells(self):
        t = FakeTarget()
        r = send_recv(t, PKT_GET_CELLS, b'\x00')
        count = struct.unpack_from('<H', r['payload'], 0)[0]
        assert count == TOTAL_CELL_COUNT

    def test_default_cells_are_3700mv(self):
        t = FakeTarget()
        r = send_recv(t, PKT_GET_CELLS, b'\x00')
        mv = struct.unpack_from('<H', r['payload'], 2)[0]
        assert mv == 3700

    def test_injected_cell_voltage_reflected(self):
        t = FakeTarget()
        t.set_cell_mv([3800] * 75)
        r = send_recv(t, PKT_GET_CELLS, b'\x00')
        mv = struct.unpack_from('<H', r['payload'], 2)[0]
        assert mv == 3800


class TestGetTemps:
    def test_returns_75_temps(self):
        t = FakeTarget()
        r = send_recv(t, PKT_GET_TEMPS)
        count = struct.unpack_from('<H', r['payload'], 0)[0]
        assert count == TOTAL_TEMP_COUNT

    def test_default_temps_are_250_cx10(self):
        t = FakeTarget()
        r = send_recv(t, PKT_GET_TEMPS)
        t_val = struct.unpack_from('<h', r['payload'], 2)[0]
        assert t_val == 250  # 25.0°C


class TestFaults:
    def test_no_faults_by_default(self):
        t = FakeTarget()
        r = send_recv(t, PKT_GET_FAULTS)
        active = struct.unpack_from('<Q', r['payload'], 0)[0]
        assert active == 0

    def test_injected_fault_appears_in_active(self):
        t = FakeTarget()
        t.inject_fault(0)  # FAULT_BIT_CELL_OV
        r = send_recv(t, PKT_GET_FAULTS)
        active = struct.unpack_from('<Q', r['payload'], 0)[0]
        assert active & 1

    def test_clear_latched_fault_after_resolve(self):
        t = FakeTarget()
        t.inject_fault(0)
        t.clear_fault(0)  # active cleared, latched remains
        # Now clear the latched fault
        mask = struct.pack('<Q', 1)
        r = send_recv(t, PKT_CLEAR_LATCHED_FAULTS, mask)
        cleared = struct.unpack_from('<Q', r['payload'], 0)[0]
        assert cleared & 1

    def test_cannot_clear_active_fault(self):
        t = FakeTarget()
        t.inject_fault(0)  # still active
        mask = struct.pack('<Q', 1)
        r = send_recv(t, PKT_CLEAR_LATCHED_FAULTS, mask)
        cleared = struct.unpack_from('<Q', r['payload'], 0)[0]
        assert not (cleared & 1)  # should not be cleared


class TestConfig:
    def test_get_config_returns_226_bytes(self):
        t = FakeTarget()
        r = send_recv(t, PKT_GET_CONFIG)
        assert len(r['payload']) == 226

    def test_validate_config_default_passes(self):
        t = FakeTarget()
        cfg = BmsConfig().pack()
        r = send_recv(t, PKT_VALIDATE_CONFIG, cfg)
        assert r['payload'][0] == 0  # result OK

    def test_set_config_ram_applies(self):
        t = FakeTarget()
        cfg = BmsConfig()
        cfg.can_base_id = 0x0123
        r = send_recv(t, PKT_SET_CONFIG_RAM, cfg.pack())
        assert r['payload'][0] == 0
        r2 = send_recv(t, PKT_GET_CONFIG)
        cfg2 = BmsConfig.unpack(r2['payload'])
        assert cfg2.can_base_id == 0x0123

    def test_store_config_persists_and_bumps_generation(self):
        t = FakeTarget()
        cfg = BmsConfig()
        gen_before = t._config.config_generation
        r = send_recv(t, PKT_STORE_CONFIG, cfg.pack())
        assert not r['is_error']
        assert r['payload'][0] == 0
        assert t._config.config_generation == gen_before + 1

    def test_store_config_rejects_invalid(self):
        t = FakeTarget()
        cfg = BmsConfig(cell_uv_hard_mv=5000)  # breaks INV-01 ordering
        r = send_recv(t, PKT_STORE_CONFIG, cfg.pack())
        assert r['is_error']

    def test_unknown_packet_returns_error(self):
        t = FakeTarget()
        r = send_recv(t, 0xFFFF)
        assert r['is_error']
