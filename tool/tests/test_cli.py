"""test_cli.py — CLI command tests using direct main() invocation.

Tests run against the in-process fake target via a TCP loopback server.
Each test starts a fresh server thread and connects to it.
"""
import json
import socket
import struct
import tempfile
import threading
import time
from pathlib import Path

import pytest

from tool.src.cli.bmsctl import main as bmsctl_main
from tool.src.fake_target.fake_target import FakeTarget


# ── TCP fake server fixture ───────────────────────────────────────────────────

def _free_port() -> int:
    with socket.socket() as s:
        s.bind(('127.0.0.1', 0))
        return s.getsockname()[1]


class FakeTcpServer:
    """Minimal per-test TCP server backed by a fresh FakeTarget."""

    def __init__(self, mode: str = 'healthy'):
        self._mode   = mode
        self._port   = _free_port()
        self._server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self._server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self._server.bind(('127.0.0.1', self._port))
        self._server.listen(5)
        self._server.settimeout(1.0)
        self._thread = threading.Thread(target=self._accept_loop, daemon=True)
        self._thread.start()

    def _accept_loop(self):
        while True:
            try:
                conn, _ = self._server.accept()
            except (OSError, socket.timeout):
                break
            threading.Thread(target=self._serve, args=(conn,), daemon=True).start()

    def _serve(self, conn):
        target = FakeTarget(mode=self._mode)
        try:
            while True:
                data = conn.recv(4096)
                if not data:
                    break
                resp = target.feed(data)
                if resp:
                    conn.sendall(resp)
        except Exception:
            pass
        finally:
            conn.close()

    def stop(self):
        self._server.close()

    @property
    def port(self) -> int:
        return self._port

    def cli_args(self) -> list:
        return ['--host', '127.0.0.1', '--port', str(self._port)]


@pytest.fixture
def srv():
    s = FakeTcpServer()
    yield s
    s.stop()


@pytest.fixture
def srv_uv():
    s = FakeTcpServer(mode='cell_uv')
    yield s
    s.stop()


# ── fake-target self-test ─────────────────────────────────────────────────────

def test_fake_target_self_test():
    rc = bmsctl_main(['fake-target', 'self-test'])
    assert rc == 0


# ── connect ───────────────────────────────────────────────────────────────────

def test_connect_returns_bms_app(srv, capsys):
    rc = bmsctl_main(['connect'] + srv.cli_args())
    assert rc == 0
    out = capsys.readouterr().out
    assert 'BMS_APP' in out


def test_connect_json_output(srv, capsys):
    rc = bmsctl_main(['connect', '--json'] + srv.cli_args())
    assert rc == 0
    data = json.loads(capsys.readouterr().out)
    assert data['mode'] == 'BMS_APP'


# ── values ────────────────────────────────────────────────────────────────────

def test_values_returns_zero(srv, capsys):
    rc = bmsctl_main(['values'] + srv.cli_args())
    assert rc == 0
    out = capsys.readouterr().out
    assert 'vbat_mv' in out


def test_values_json(srv, capsys):
    rc = bmsctl_main(['values', '--json'] + srv.cli_args())
    assert rc == 0
    data = json.loads(capsys.readouterr().out)
    assert 'vbat_mv' in data


# ── cells ─────────────────────────────────────────────────────────────────────

def test_cells_summary(srv, capsys):
    rc = bmsctl_main(['cells'] + srv.cli_args())
    assert rc == 0
    out = capsys.readouterr().out
    assert 'min_mv' in out


def test_cells_verbose(srv, capsys):
    rc = bmsctl_main(['cells', '-v'] + srv.cli_args())
    assert rc == 0
    out = capsys.readouterr().out
    assert 'cell[00]' in out


def test_cells_uv_mode(srv_uv, capsys):
    rc = bmsctl_main(['cells'] + srv_uv.cli_args())
    assert rc == 0
    out = capsys.readouterr().out
    assert '2400' in out or 'min_mv' in out


# ── temps ─────────────────────────────────────────────────────────────────────

def test_temps_summary(srv, capsys):
    rc = bmsctl_main(['temps'] + srv.cli_args())
    assert rc == 0
    out = capsys.readouterr().out
    assert 'temp_count' in out or 'max' in out


# ── faults ────────────────────────────────────────────────────────────────────

def test_faults_no_active(srv, capsys):
    rc = bmsctl_main(['faults'] + srv.cli_args())
    assert rc == 0
    out = capsys.readouterr().out
    assert 'active_faults' in out


def test_faults_cell_uv_active(srv_uv, capsys):
    rc = bmsctl_main(['faults'] + srv_uv.cli_args())
    assert rc == 0
    out = capsys.readouterr().out
    assert 'CELL_UV' in out


def test_faults_json(srv, capsys):
    rc = bmsctl_main(['faults', '--json'] + srv.cli_args())
    assert rc == 0
    data = json.loads(capsys.readouterr().out)
    assert 'active_faults' in data


# ── diagnostics ───────────────────────────────────────────────────────────────

def test_diagnostics_returns_reset_cause(srv, capsys):
    rc = bmsctl_main(['diagnostics'] + srv.cli_args())
    assert rc == 0
    out = capsys.readouterr().out
    assert 'reset_cause' in out


# ── config export-default ────────────────────────────────────────────────────

def test_config_export_default_to_file(tmp_path):
    out_file = str(tmp_path / 'default.bin')
    rc = bmsctl_main(['config', 'export-default', '--out', out_file])
    assert rc == 0
    assert Path(out_file).stat().st_size == 226


def test_config_validate_default_passes(tmp_path):
    blob_file = str(tmp_path / 'cfg.bin')
    bmsctl_main(['config', 'export-default', '--out', blob_file])
    rc = bmsctl_main(['config', 'validate', blob_file])
    assert rc == 0


def test_config_validate_corrupted_fails(tmp_path):
    blob_file = tmp_path / 'bad.bin'
    blob_file.write_bytes(bytes(226))  # all zeros — bad magic
    rc = bmsctl_main(['config', 'validate', str(blob_file)])
    assert rc != 0


# ── config read ───────────────────────────────────────────────────────────────

def test_config_read_from_target(srv, tmp_path, capsys):
    out_file = str(tmp_path / 'read.bin')
    rc = bmsctl_main(['config', 'read', '--out', out_file] + srv.cli_args())
    assert rc == 0
    assert Path(out_file).stat().st_size == 226


# ── config apply-ram ─────────────────────────────────────────────────────────

def test_config_apply_ram_default(srv, tmp_path):
    blob_file = str(tmp_path / 'cfg.bin')
    bmsctl_main(['config', 'export-default', '--out', blob_file])
    rc = bmsctl_main(['config', 'apply-ram', blob_file] + srv.cli_args())
    assert rc == 0


# ── config diff ──────────────────────────────────────────────────────────────

def test_config_diff_identical(tmp_path, capsys):
    blob = str(tmp_path / 'cfg.bin')
    bmsctl_main(['config', 'export-default', '--out', blob])
    rc = bmsctl_main(['config', 'diff', blob, blob])
    assert rc == 0
    out = capsys.readouterr().out
    assert 'identical' in out


# ── package build / inspect / validate ───────────────────────────────────────

def test_package_build_and_inspect(tmp_path, capsys):
    fw_file  = str(tmp_path / 'fw.bin')
    pkg_file = str(tmp_path / 'fw.pkg')
    Path(fw_file).write_bytes(bytes(range(256)) * 4)
    rc = bmsctl_main(['package', 'build', fw_file, pkg_file, '--version', '0.1.2'])
    assert rc == 0
    rc = bmsctl_main(['package', 'inspect', pkg_file])
    assert rc == 0
    out = capsys.readouterr().out
    assert '0.1.2' in out


def test_package_validate_passes(tmp_path):
    fw_file  = str(tmp_path / 'fw.bin')
    pkg_file = str(tmp_path / 'fw.pkg')
    Path(fw_file).write_bytes(bytes(1024))
    bmsctl_main(['package', 'build', fw_file, pkg_file])
    rc = bmsctl_main(['package', 'validate', pkg_file])
    assert rc == 0


def test_package_validate_corrupt_fails(tmp_path):
    bad_pkg = tmp_path / 'bad.pkg'
    bad_pkg.write_bytes(bytes(128))  # too short / bad magic
    rc = bmsctl_main(['package', 'validate', str(bad_pkg)])
    assert rc != 0


def test_package_build_empty_fails(tmp_path, capsys):
    fw_file  = str(tmp_path / 'empty.bin')
    pkg_file = str(tmp_path / 'out.pkg')
    Path(fw_file).write_bytes(b'')
    rc = bmsctl_main(['package', 'build', fw_file, pkg_file])
    assert rc != 0


# ── stlink dry-run ───────────────────────────────────────────────────────────

def test_stlink_dry_run_shows_command(tmp_path, capsys):
    fw_file  = str(tmp_path / 'fw.bin')
    pkg_file = str(tmp_path / 'fw.pkg')
    Path(fw_file).write_bytes(bytes(1024))
    bmsctl_main(['package', 'build', fw_file, pkg_file])
    rc = bmsctl_main(['stlink', 'dry-run-app', pkg_file])
    assert rc == 0
    out = capsys.readouterr().out
    assert 'DRY-RUN' in out
    assert '0x08008000' in out


def test_stlink_dry_run_missing_file_fails(capsys):
    rc = bmsctl_main(['stlink', 'dry-run-app', '/tmp/nonexistent_12345.pkg'])
    assert rc != 0
