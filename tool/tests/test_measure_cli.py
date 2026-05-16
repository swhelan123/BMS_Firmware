"""test_measure_cli.py — Tests for one-shot measurement CLI commands.

All tests run against FakeTargetInProcess (in-process, no TCP).
Covers: measure cells, measure temps, measure power.
"""
import struct
import pytest

from tool.src.fake_target.fake_target import FakeTargetInProcess
from tool.src.protocol.client import BmsProtocolClient, ProtocolError
from tool.src.core.target_model import TargetModel, TargetRefusedError
from tool.src.cli.bmsctl import (
    cmd_measure_cells, cmd_measure_temps, cmd_measure_power,
    main,
)

TEMP_INVALID_CX10 = -0x8000
TOTAL_CELL_COUNT  = 75
TOTAL_TEMP_COUNT  = 75

FAULT_BIT_ISOSPI_CELL   = 15
FAULT_BIT_ISOSPI_TEMP   = 16
FAULT_BIT_I2C_ISL28022  = 17
FAULT_BIT_VPACK_INVALID = 12


# ── helpers ───────────────────────────────────────────────────────────────────

class _FakePort:
    def __init__(self, mode: str = 'healthy'):
        self._ft  = FakeTargetInProcess(mode=mode)
        self._buf = b''

    def write(self, data: bytes) -> None:
        self._buf += self._ft.exchange(data)

    def read(self, n: int) -> bytes:
        chunk, self._buf = self._buf[:n], self._buf[n:]
        return chunk

    @property
    def in_waiting(self) -> int:
        return len(self._buf)


def _model(mode: str = 'healthy') -> TargetModel:
    m = TargetModel(_FakePort(mode))
    m.capabilities_handshake()
    return m


class _Args:
    host   = '127.0.0.1'
    port   = 65102
    serial = None
    baud   = 115200
    json   = False

    def __init__(self, mode: str = 'healthy', **kw):
        self._mode = mode
        for k, v in kw.items():
            setattr(self, k, v)


def _make_connect(mode: str):
    from tool.src.core.connection_manager import ConnectionManager
    class _FakeMgr:
        def disconnect(self): pass
    def _connect_patch(args):
        m = TargetModel(_FakePort(mode))
        m.capabilities_handshake()
        return _FakeMgr(), m
    return _connect_patch


import tool.src.cli.bmsctl as _bmsctl_mod


# ── measure cells ─────────────────────────────────────────────────────────────

class TestMeasureCells:
    def test_healthy_status_ok(self):
        r = _model().measure_cells_once()
        assert r['status'] == 0

    def test_healthy_cell_count_75(self):
        r = _model().measure_cells_once()
        assert r['cell_count'] == TOTAL_CELL_COUNT
        assert len(r['cells_mv']) == TOTAL_CELL_COUNT

    def test_healthy_default_voltage_3700(self):
        r = _model().measure_cells_once()
        assert all(mv == 3700 for mv in r['cells_mv'])

    def test_healthy_all_valid(self):
        r = _model().measure_cells_once()
        assert all(r['validity'])

    def test_isospi_fault_status_fail(self):
        r = _model('isospi_fault').measure_cells_once()
        assert r['status'] != 0

    def test_isospi_fault_zero_voltage(self):
        r = _model('isospi_fault').measure_cells_once()
        assert all(mv == 0 for mv in r['cells_mv'])

    def test_isospi_fault_no_valid_cells(self):
        r = _model('isospi_fault').measure_cells_once()
        assert not any(r['validity'])

    def test_cell_uv_mode_has_low_cell(self):
        r = _model('cell_uv').measure_cells_once()
        assert r['cells_mv'][0] == 2400

    def test_cell_ov_mode_has_high_cell(self):
        r = _model('cell_ov').measure_cells_once()
        assert r['cells_mv'][0] == 4300

    def test_bootloader_raises_refused(self):
        with pytest.raises(TargetRefusedError):
            _model('bootloader').measure_cells_once()

    def test_cli_exit_0_healthy(self, monkeypatch):
        monkeypatch.setattr(_bmsctl_mod, '_connect', _make_connect('healthy'))
        assert cmd_measure_cells(_Args()) == 0

    def test_cli_exit_1_isospi_fault(self, monkeypatch):
        monkeypatch.setattr(_bmsctl_mod, '_connect', _make_connect('isospi_fault'))
        assert cmd_measure_cells(_Args()) == 1

    def test_cli_json_has_cells_mv(self, monkeypatch, capsys):
        import json
        monkeypatch.setattr(_bmsctl_mod, '_connect', _make_connect('healthy'))
        cmd_measure_cells(_Args(json=True))
        data = json.loads(capsys.readouterr().out)
        assert 'cells_mv' in data
        assert len(data['cells_mv']) == TOTAL_CELL_COUNT

    def test_cli_json_has_validity(self, monkeypatch, capsys):
        import json
        monkeypatch.setattr(_bmsctl_mod, '_connect', _make_connect('healthy'))
        cmd_measure_cells(_Args(json=True))
        data = json.loads(capsys.readouterr().out)
        assert 'validity' in data
        assert len(data['validity']) == TOTAL_CELL_COUNT

    def test_cli_json_has_timestamp(self, monkeypatch, capsys):
        import json
        monkeypatch.setattr(_bmsctl_mod, '_connect', _make_connect('healthy'))
        cmd_measure_cells(_Args(json=True))
        data = json.loads(capsys.readouterr().out)
        assert 'timestamp_ms' in data


# ── measure temps ─────────────────────────────────────────────────────────────

class TestMeasureTemps:
    def test_healthy_status_ok(self):
        r = _model().measure_temps_once()
        assert r['status'] == 0

    def test_healthy_temp_count_75(self):
        r = _model().measure_temps_once()
        assert r['temp_count'] == TOTAL_TEMP_COUNT
        assert len(r['temps_cx10']) == TOTAL_TEMP_COUNT

    def test_healthy_default_temp_250(self):
        r = _model().measure_temps_once()
        assert all(t == 250 for t in r['temps_cx10'])

    def test_temp_invalid_mode_all_invalid(self):
        r = _model('temp_invalid').measure_temps_once()
        # temp_invalid mode uses healthy isoSPI but sets temps to INVALID sentinel
        assert all(t == TEMP_INVALID_CX10 for t in r['temps_cx10'])

    def test_isospi_temp_fault_status_fail(self):
        # Inject ISOSPI_TEMP fault by using a model that has it
        ft = FakeTargetInProcess(mode='healthy')
        ft.inject_fault(FAULT_BIT_ISOSPI_TEMP)
        port = _FakePort.__new__(_FakePort)
        port._ft  = ft
        port._buf = b''
        m = TargetModel(port)
        m.capabilities_handshake()
        r = m.measure_temps_once()
        assert r['status'] != 0

    def test_bootloader_raises_refused(self):
        with pytest.raises(TargetRefusedError):
            _model('bootloader').measure_temps_once()

    def test_cli_exit_0_healthy(self, monkeypatch):
        monkeypatch.setattr(_bmsctl_mod, '_connect', _make_connect('healthy'))
        assert cmd_measure_temps(_Args()) == 0

    def test_cli_json_schema(self, monkeypatch, capsys):
        import json
        monkeypatch.setattr(_bmsctl_mod, '_connect', _make_connect('healthy'))
        cmd_measure_temps(_Args(json=True))
        data = json.loads(capsys.readouterr().out)
        assert 'status' in data
        assert 'temp_count' in data
        assert 'temps_cx10' in data
        assert 'timestamp_ms' in data
        assert len(data['temps_cx10']) == TOTAL_TEMP_COUNT


# ── measure power ─────────────────────────────────────────────────────────────

class TestMeasurePower:
    def test_healthy_status_ok(self):
        r = _model().measure_power_once()
        assert r['status'] == 0

    def test_healthy_vbat_valid(self):
        r = _model().measure_power_once()
        assert r['vbat_valid'] is True

    def test_healthy_vpack_valid(self):
        r = _model().measure_power_once()
        assert r['vpack_valid'] is True

    def test_healthy_i_batt_valid(self):
        r = _model().measure_power_once()
        assert r['i_batt_valid'] is True

    def test_healthy_vbat_nonzero(self):
        r = _model().measure_power_once()
        assert r['vbat_mv'] > 0

    def test_vpack_invalid_status_fail(self):
        r = _model('vpack_invalid').measure_power_once()
        assert r['status'] != 0

    def test_vpack_invalid_vpack_not_valid(self):
        r = _model('vpack_invalid').measure_power_once()
        assert r['vpack_valid'] is False

    def test_i2c_fault_vbat_invalid(self):
        ft = FakeTargetInProcess(mode='healthy')
        ft.inject_fault(FAULT_BIT_I2C_ISL28022)
        port = _FakePort.__new__(_FakePort)
        port._ft  = ft
        port._buf = b''
        m = TargetModel(port)
        m.capabilities_handshake()
        r = m.measure_power_once()
        assert r['vbat_valid'] is False
        assert r['i_batt_valid'] is False

    def test_bootloader_raises_refused(self):
        with pytest.raises(TargetRefusedError):
            _model('bootloader').measure_power_once()

    def test_cli_exit_0_healthy(self, monkeypatch):
        monkeypatch.setattr(_bmsctl_mod, '_connect', _make_connect('healthy'))
        assert cmd_measure_power(_Args()) == 0

    def test_cli_exit_1_vpack_fault(self, monkeypatch):
        monkeypatch.setattr(_bmsctl_mod, '_connect', _make_connect('vpack_invalid'))
        assert cmd_measure_power(_Args()) == 1

    def test_cli_json_schema(self, monkeypatch, capsys):
        import json
        monkeypatch.setattr(_bmsctl_mod, '_connect', _make_connect('healthy'))
        cmd_measure_power(_Args(json=True))
        data = json.loads(capsys.readouterr().out)
        for key in ('status', 'vbat_mv', 'vpack_mv', 'i_batt_ma',
                    'vbat_valid', 'vpack_valid', 'i_batt_valid', 'timestamp_ms'):
            assert key in data

    def test_flags_byte_encodes_validity(self):
        r = _model().measure_power_once()
        # All valid → flags = 0b111 = 7
        assert r['flags'] == 0x07


# ── safety gates ──────────────────────────────────────────────────────────────

class TestMeasureSafetyGates:
    def test_measure_cells_requires_app_mode(self):
        m = TargetModel(_FakePort('bootloader'))
        m.capabilities_handshake()
        with pytest.raises(TargetRefusedError):
            m.measure_cells_once()

    def test_measure_temps_requires_app_mode(self):
        m = TargetModel(_FakePort('bootloader'))
        m.capabilities_handshake()
        with pytest.raises(TargetRefusedError):
            m.measure_temps_once()

    def test_measure_power_requires_app_mode(self):
        m = TargetModel(_FakePort('bootloader'))
        m.capabilities_handshake()
        with pytest.raises(TargetRefusedError):
            m.measure_power_once()

    def test_multiple_measure_calls_succeed(self):
        m = _model()
        for _ in range(3):
            r = m.measure_cells_once()
            assert r['status'] == 0

    def test_measure_cells_after_temps_ok(self):
        m = _model()
        assert m.measure_temps_once()['status'] == 0
        assert m.measure_cells_once()['status'] == 0
