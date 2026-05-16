"""test_openwire_cli.py — Tests for bmsctl openwire run and fake target openwire modes.

All tests run against FakeTargetInProcess (in-process, no TCP).
Covers: healthy (no wires), openwire_detected, openwire_pec_fail,
        bootloader refusal, CLI JSON schema.
"""
import pytest

from tool.src.fake_target.fake_target import FakeTargetInProcess
from tool.src.core.target_model import TargetModel, TargetRefusedError
from tool.src.protocol.client import ProtocolError
from tool.src.cli.bmsctl import cmd_openwire_run, main

TOTAL_CELL_COUNT = 75


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


# ── run_openwire model API tests ──────────────────────────────────────────────

class TestRunOpenwire:
    def test_healthy_status_ok(self):
        r = _model().run_openwire()
        assert r['status'] == 0

    def test_healthy_mask_all_zero(self):
        r = _model().run_openwire()
        assert r['open_wire_mask'] == b'\x00' * 10

    def test_openwire_detected_status_ok(self):
        r = _model('openwire_detected').run_openwire()
        assert r['status'] == 0

    def test_openwire_detected_cell0_flagged(self):
        r = _model('openwire_detected').run_openwire()
        # cell 0 is in byte 0, bit 0
        assert r['open_wire_mask'][0] & 0x01

    def test_openwire_detected_mask_not_all_zero(self):
        r = _model('openwire_detected').run_openwire()
        assert r['open_wire_mask'] != b'\x00' * 10

    def test_openwire_pec_fail_status_nonzero(self):
        r = _model('openwire_pec_fail').run_openwire()
        assert r['status'] != 0

    def test_openwire_pec_fail_mask_all_zero(self):
        r = _model('openwire_pec_fail').run_openwire()
        assert r['open_wire_mask'] == b'\x00' * 10

    def test_bootloader_raises_refused(self):
        with pytest.raises(TargetRefusedError):
            _model('bootloader').run_openwire()

    def test_multiple_calls_succeed(self):
        m = _model()
        for _ in range(3):
            r = m.run_openwire()
            assert r['status'] == 0


# ── CLI tests ─────────────────────────────────────────────────────────────────

class TestOpenwireCli:
    def test_cli_exit_0_healthy(self, monkeypatch):
        monkeypatch.setattr(_bmsctl_mod, '_connect', _make_connect('healthy'))
        assert cmd_openwire_run(_Args()) == 0

    def test_cli_exit_1_openwire_detected(self, monkeypatch):
        monkeypatch.setattr(_bmsctl_mod, '_connect', _make_connect('openwire_detected'))
        assert cmd_openwire_run(_Args()) == 1

    def test_cli_exit_1_pec_fail(self, monkeypatch):
        monkeypatch.setattr(_bmsctl_mod, '_connect', _make_connect('openwire_pec_fail'))
        assert cmd_openwire_run(_Args()) == 1

    def test_cli_json_schema_healthy(self, monkeypatch, capsys):
        import json
        monkeypatch.setattr(_bmsctl_mod, '_connect', _make_connect('healthy'))
        cmd_openwire_run(_Args(json=True))
        data = json.loads(capsys.readouterr().out)
        assert 'status' in data
        assert 'open_wire_mask' in data
        assert 'detected_count' in data
        assert 'detected_cells' in data

    def test_cli_json_healthy_detected_count_zero(self, monkeypatch, capsys):
        import json
        monkeypatch.setattr(_bmsctl_mod, '_connect', _make_connect('healthy'))
        cmd_openwire_run(_Args(json=True))
        data = json.loads(capsys.readouterr().out)
        assert data['detected_count'] == 0
        assert data['detected_cells'] == []

    def test_cli_json_detected_has_cell0(self, monkeypatch, capsys):
        import json
        monkeypatch.setattr(_bmsctl_mod, '_connect', _make_connect('openwire_detected'))
        cmd_openwire_run(_Args(json=True))
        data = json.loads(capsys.readouterr().out)
        assert data['detected_count'] >= 1
        assert 0 in data['detected_cells']
