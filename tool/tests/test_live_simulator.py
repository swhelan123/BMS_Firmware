"""test_live_simulator.py — Tests for LiveFakeHardware.

Uses manual tick() calls to control time; no wall-clock sleeps needed.
"""
import pytest
import struct

from tool.src.fake_target.live_simulator import LiveFakeHardware, LIVE_MODES
from tool.src.fake_target.fake_target import (
    FAULT_BIT_CELL_UV, FAULT_BIT_CELL_OV, FAULT_BIT_ISOSPI_CELL,
    FAULT_BIT_VPACK_INVALID,
)
from tool.src.protocol.framing import FrameDecoder, encode_frame
from tool.src.protocol.packet_defs import (
    PKT_GET_CAPABILITIES, PKT_GET_VALUES, PKT_GET_CELLS, PKT_GET_TEMPS,
    PKT_GET_FAULTS, PKT_GET_DIAGNOSTICS_SUMMARY, PKT_RUN_OPENWIRE,
    FIRMWARE_TYPE_BOOTLOADER, TOTAL_CELL_COUNT, TOTAL_TEMP_COUNT,
)


# ── Helpers ───────────────────────────────────────────────────────────────────

def make_sim(mode, seed=42, tick_interval=9999.0):
    """Create a LiveFakeHardware with a very long tick interval so the background
    thread never fires during a test; call sim.tick() manually instead."""
    return LiveFakeHardware(mode=mode, seed=seed, tick_interval=tick_interval)


def exchange(sim, pkt_id, payload=b''):
    """Send one request to the simulator and return the decoded response payload."""
    frame = encode_frame(pkt_id, payload)
    decoder = FrameDecoder()
    resp_bytes = sim.feed(frame, decoder)
    frames = FrameDecoder().feed(resp_bytes)
    assert len(frames) == 1, f"Expected 1 response frame, got {len(frames)}"
    assert frames[0]['is_response']
    return frames[0]['payload']


# ── Known-modes coverage ──────────────────────────────────────────────────────

def test_all_live_modes_exist():
    assert 'healthy-idle' in LIVE_MODES
    assert 'drive' in LIVE_MODES
    assert 'charge' in LIVE_MODES
    assert 'cell-uv' in LIVE_MODES
    assert 'cell-ov' in LIVE_MODES
    assert 'temp-high' in LIVE_MODES
    assert 'isospi-fault' in LIVE_MODES
    assert 'openwire-detected' in LIVE_MODES
    assert 'vpack-invalid' in LIVE_MODES
    assert 'bootloader' in LIVE_MODES
    assert len(LIVE_MODES) == 10


def test_unknown_mode_raises():
    with pytest.raises(ValueError, match="Unknown live mode"):
        LiveFakeHardware(mode='nonexistent', tick_interval=9999.0)


# ── healthy-idle ──────────────────────────────────────────────────────────────

class TestHealthyIdle:
    def test_capabilities_returns_app_type(self):
        sim = make_sim('healthy-idle')
        pl = exchange(sim, PKT_GET_CAPABILITIES)
        fw_type = struct.unpack_from('<H', pl, 0)[0]
        assert fw_type != FIRMWARE_TYPE_BOOTLOADER

    def test_no_faults_initially(self):
        sim = make_sim('healthy-idle')
        assert sim.get_active_faults() == 0

    def test_cells_near_3700(self):
        sim = make_sim('healthy-idle')
        cells = sim.get_cell_mv()
        assert len(cells) == TOTAL_CELL_COUNT
        for mv in cells:
            assert 3680 <= mv <= 3720, f"unexpected cell voltage {mv}"

    def test_temps_near_25(self):
        sim = make_sim('healthy-idle')
        temps = sim.get_temp_cx10()
        assert len(temps) == TOTAL_TEMP_COUNT
        for t in temps:
            assert 200 <= t <= 300, f"unexpected temp {t}"

    def test_uptime_advances_with_ticks(self):
        sim = make_sim('healthy-idle', tick_interval=0.1)
        sim.tick()
        sim.tick()
        assert sim.get_uptime_ms() > 0

    def test_cells_drift_after_tick(self):
        sim = make_sim('healthy-idle', seed=0)
        before = sim.get_cell_mv()
        sim.tick()
        after = sim.get_cell_mv()
        # With seed=0 some cells should have drifted (not all exactly the same)
        assert before != after  # drift occurs

    def test_deterministic_with_same_seed(self):
        sim_a = make_sim('healthy-idle', seed=7)
        sim_b = make_sim('healthy-idle', seed=7)
        for _ in range(5):
            sim_a.tick()
            sim_b.tick()
        assert sim_a.get_cell_mv() == sim_b.get_cell_mv()

    def test_different_seeds_produce_different_drift(self):
        sim_a = make_sim('healthy-idle', seed=1)
        sim_b = make_sim('healthy-idle', seed=2)
        for _ in range(10):
            sim_a.tick()
            sim_b.tick()
        assert sim_a.get_cell_mv() != sim_b.get_cell_mv()


# ── drive ─────────────────────────────────────────────────────────────────────

class TestDrive:
    def test_cells_drain_over_ticks(self):
        sim = make_sim('drive')
        initial = sim.get_cell_mv()
        # Drain occurs every 2 ticks
        for _ in range(4):
            sim.tick()
        after = sim.get_cell_mv()
        assert after[0] < initial[0], "cells should drain in drive mode"

    def test_cells_do_not_go_below_floor(self):
        sim = make_sim('drive')
        for _ in range(10000):
            sim.tick()
        assert all(mv >= 2800 for mv in sim.get_cell_mv())

    def test_protocol_responds(self):
        sim = make_sim('drive')
        pl = exchange(sim, PKT_GET_VALUES)
        assert len(pl) > 0


# ── charge ────────────────────────────────────────────────────────────────────

class TestCharge:
    def test_cells_rise_over_ticks(self):
        sim = make_sim('charge')
        initial = sim.get_cell_mv()
        for _ in range(4):
            sim.tick()
        after = sim.get_cell_mv()
        assert after[0] > initial[0], "cells should rise in charge mode"

    def test_cells_do_not_exceed_ceiling(self):
        sim = make_sim('charge')
        for _ in range(10000):
            sim.tick()
        assert all(mv <= 4100 for mv in sim.get_cell_mv())


# ── cell-uv ───────────────────────────────────────────────────────────────────

class TestCellUv:
    def test_no_fault_initially(self):
        sim = make_sim('cell-uv')
        assert sim.get_active_faults() == 0

    def test_fault_triggers_when_cell_hits_threshold(self):
        sim = make_sim('cell-uv')
        # cell[0] starts at 3700, drains 2 mV every 2 ticks
        # Need (3700 - 2500) / 2 * 2 = 1200 ticks to reach threshold
        for _ in range(1300):
            sim.tick()
        assert sim.get_active_faults() & (1 << FAULT_BIT_CELL_UV), \
            "FAULT_CELL_UV should be active"

    def test_cell0_drains_faster_than_others(self):
        sim = make_sim('cell-uv')
        for _ in range(10):
            sim.tick()
        cells = sim.get_cell_mv()
        # cell[0] should have drained; others stay at 3700
        assert cells[0] < cells[1]


# ── cell-ov ───────────────────────────────────────────────────────────────────

class TestCellOv:
    def test_no_fault_initially(self):
        sim = make_sim('cell-ov')
        assert sim.get_active_faults() == 0

    def test_fault_triggers_when_cell_hits_threshold(self):
        sim = make_sim('cell-ov')
        # (4200 - 3700) / 2 * 2 = 500 ticks to trigger
        for _ in range(600):
            sim.tick()
        assert sim.get_active_faults() & (1 << FAULT_BIT_CELL_OV), \
            "FAULT_CELL_OV should be active"


# ── temp-high ─────────────────────────────────────────────────────────────────

class TestTempHigh:
    def test_temps_rise(self):
        sim = make_sim('temp-high')
        initial = sim.get_temp_cx10()
        for _ in range(10):
            sim.tick()
        after = sim.get_temp_cx10()
        assert after[0] > initial[0]

    def test_temps_plateau(self):
        sim = make_sim('temp-high')
        for _ in range(10000):
            sim.tick()
        assert all(t <= 450 for t in sim.get_temp_cx10())


# ── isospi-fault ──────────────────────────────────────────────────────────────

class TestIsospiFault:
    def test_isospi_fault_active_immediately(self):
        sim = make_sim('isospi-fault')
        assert sim.get_active_faults() & (1 << FAULT_BIT_ISOSPI_CELL)

    def test_cells_still_valid(self):
        sim = make_sim('isospi-fault')
        cells = sim.get_cell_mv()
        assert all(mv > 0 for mv in cells)

    def test_fault_persists_after_ticks(self):
        sim = make_sim('isospi-fault')
        for _ in range(5):
            sim.tick()
        assert sim.get_active_faults() & (1 << FAULT_BIT_ISOSPI_CELL)


# ── openwire-detected ─────────────────────────────────────────────────────────

class TestOpenwireDetected:
    def test_openwire_scan_returns_detected(self):
        sim = make_sim('openwire-detected')
        pl = exchange(sim, PKT_RUN_OPENWIRE)
        # status=0 (success), then bitmask of detected cells
        assert pl[0] == 0, "openwire scan status should be 0 (success)"
        # cell[0] should be flagged: bit 0 of byte 1
        assert pl[1] & 0x01, "cell[0] should be flagged as open wire"

    def test_cells_otherwise_valid(self):
        sim = make_sim('openwire-detected')
        cells = sim.get_cell_mv()
        assert cells[1] == 3700


# ── vpack-invalid ─────────────────────────────────────────────────────────────

class TestVpackInvalid:
    def test_vpack_fault_active(self):
        sim = make_sim('vpack-invalid')
        assert sim.get_active_faults() & (1 << FAULT_BIT_VPACK_INVALID)


# ── bootloader ────────────────────────────────────────────────────────────────

class TestBootloader:
    def test_capabilities_returns_bootloader_type(self):
        sim = make_sim('bootloader')
        pl = exchange(sim, PKT_GET_CAPABILITIES)
        fw_type = struct.unpack_from('<H', pl, 0)[0]
        assert fw_type == FIRMWARE_TYPE_BOOTLOADER

    def test_uptime_still_ticks(self):
        sim = make_sim('bootloader', tick_interval=0.1)
        sim.tick()
        sim.tick()
        assert sim.get_uptime_ms() > 0


# ── Thread safety ─────────────────────────────────────────────────────────────

class TestThreadSafety:
    def test_concurrent_feed_does_not_crash(self):
        import threading
        sim = make_sim('healthy-idle')
        errors = []

        def worker():
            try:
                decoder = FrameDecoder()
                for _ in range(50):
                    frame = encode_frame(PKT_GET_VALUES, b'')
                    sim.feed(frame, decoder)
                    sim.tick()
            except Exception as exc:
                errors.append(exc)

        threads = [threading.Thread(target=worker) for _ in range(4)]
        for t in threads:
            t.start()
        for t in threads:
            t.join()
        assert errors == [], f"Thread errors: {errors}"

    def test_stop_stops_tick_thread(self):
        sim = LiveFakeHardware(mode='healthy-idle', tick_interval=0.05)
        sim.stop()
        # Thread should exit on next iteration; just check no exception
        assert not sim._running
