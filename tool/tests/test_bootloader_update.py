"""test_bootloader_update.py — tests for the bootloader update protocol simulation.

Covers:
- FakeTarget bootloader mode transitions (enter_bootloader → BOOTLOADER)
- BOOT_UPDATE_BEGIN / CHUNK / FINALIZE / ABORT handlers
- BootloaderUpdater full flow against FakeTargetInProcess
- Error paths: wrong mode, bad header, wrong chunk index, abort mid-transfer
- bmsctl update simulate / validate / dry-run CLI commands via TCP loopback
"""
import json
import socket
import struct
import threading
import time
from pathlib import Path

import pytest

from tool.src.fake_target.fake_target import FakeTarget, FakeTargetInProcess
from tool.src.protocol.framing import encode_frame, decode_frame
from tool.src.protocol.packet_defs import (
    PKT_GET_CAPABILITIES, PKT_ENTER_BOOTLOADER,
    PKT_BOOT_UPDATE_BEGIN, PKT_BOOT_UPDATE_CHUNK,
    PKT_BOOT_UPDATE_FINALIZE, PKT_BOOT_UPDATE_ABORT,
    FIRMWARE_TYPE_BMS_APP, FIRMWARE_TYPE_BOOTLOADER,
    ENTER_BOOTLOADER_MAGIC,
)
from tool.src.protocol.client import BmsProtocolClient
from tool.src.update.package_builder import build_package
from tool.src.update.package_parser import parse_header, PKG_HEADER_SIZE
from tool.src.update.bootloader_updater import BootloaderUpdater, UpdateError
from tool.src.cli.bmsctl import main as bmsctl_main


# ── Shared in-process port ────────────────────────────────────────────────────

class InProcessPort:
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


def make_client(mode: str = 'healthy') -> tuple:
    port   = InProcessPort(mode)
    client = BmsProtocolClient(port)
    return client, port


def _small_pkg() -> bytes:
    fw = bytes(range(256)) * 4   # 1 KB
    return build_package(fw, fw_version=(0, 1, 0))


def _pkg_hdr_bytes(pkg: bytes) -> bytes:
    return pkg[:PKG_HEADER_SIZE]


# ── FakeTarget enter_bootloader transition ────────────────────────────────────

class TestEnterBootloader:
    def test_enter_bootloader_transitions_firmware_type(self):
        client, port = make_client('healthy')
        # Initially BMS_APP
        caps = client.get_capabilities()
        assert caps['firmware_type'] == FIRMWARE_TYPE_BMS_APP

        client.enter_bootloader()
        caps = client.get_capabilities()
        assert caps['firmware_type'] == FIRMWARE_TYPE_BOOTLOADER

    def test_enter_bootloader_rejects_bad_magic(self):
        client, _ = make_client('healthy')
        frame = encode_frame(PKT_ENTER_BOOTLOADER, struct.pack('<I', 0xDEADBEEF), seq=1)
        resp  = decode_frame(client._port.ft.target.feed(frame))
        assert resp['is_error']

    def test_bootloader_mode_denies_update_before_begin(self):
        client, _ = make_client('bootloader')
        # Sending CHUNK without BEGIN should return an error
        req = struct.pack('<II', 0, 4) + b'\x00' * 4
        frame = encode_frame(PKT_BOOT_UPDATE_CHUNK, req, seq=1)
        resp  = decode_frame(client._port.ft.target.feed(frame))
        assert resp['is_error']

    def test_app_mode_denies_begin(self):
        """BOOT_UPDATE_BEGIN in BMS_APP mode must error (not in bootloader)."""
        client, _ = make_client('healthy')
        pkg = _small_pkg()
        frame = encode_frame(PKT_BOOT_UPDATE_BEGIN, _pkg_hdr_bytes(pkg), seq=1)
        resp  = decode_frame(client._port.ft.target.feed(frame))
        assert resp['is_error']


# ── BOOT_UPDATE_BEGIN handler ─────────────────────────────────────────────────

class TestBootUpdateBegin:
    def setup_method(self):
        self.client, self.port = make_client('bootloader')
        self.pkg = _small_pkg()

    def test_begin_accepted_in_bootloader_mode(self):
        resp = self.client.boot_update_begin(_pkg_hdr_bytes(self.pkg))
        assert resp['result'] == 0

    def test_begin_returns_chunk_size_and_total_chunks(self):
        hdr    = parse_header(_pkg_hdr_bytes(self.pkg))
        resp   = self.client.boot_update_begin(_pkg_hdr_bytes(self.pkg))
        chunk  = resp['expected_chunk_size']
        total  = resp['total_chunks']
        assert chunk > 0
        assert total == (hdr.app_size + chunk - 1) // chunk

    def test_begin_rejects_bad_header(self):
        bad = bytes(64)   # all zeros — fails magic check
        frame = encode_frame(PKT_BOOT_UPDATE_BEGIN, bad, seq=1)
        raw   = self.port.ft.target.feed(frame)
        resp  = decode_frame(raw)
        # Either error response OR a response with result != 0
        if not resp['is_error']:
            assert resp['payload'][0] != 0   # result field


# ── BOOT_UPDATE_CHUNK handler ─────────────────────────────────────────────────

class TestBootUpdateChunk:
    def setup_method(self):
        self.client, self.port = make_client('bootloader')
        self.pkg = _small_pkg()
        self.begin = self.client.boot_update_begin(_pkg_hdr_bytes(self.pkg))
        assert self.begin['result'] == 0
        self.chunk_size = self.begin['expected_chunk_size']

    def test_first_chunk_accepted(self):
        payload = self.pkg[PKG_HEADER_SIZE:PKG_HEADER_SIZE + self.chunk_size]
        result  = self.client.boot_update_chunk(0, payload)
        assert result == 0

    def test_out_of_order_chunk_rejected(self):
        payload = b'\xAA' * min(self.chunk_size, 64)
        frame   = encode_frame(PKT_BOOT_UPDATE_CHUNK,
                               struct.pack('<II', 5, len(payload)) + payload, seq=1)
        raw  = self.port.ft.target.feed(frame)
        resp = decode_frame(raw)
        assert resp['is_error']


# ── BOOT_UPDATE_FINALIZE handler ──────────────────────────────────────────────

class TestBootUpdateFinalize:
    def _do_full_transfer(self, client, pkg):
        begin      = client.boot_update_begin(_pkg_hdr_bytes(pkg))
        chunk_size = begin['expected_chunk_size']
        payload    = pkg[PKG_HEADER_SIZE:]
        idx = 0
        offset = 0
        while offset < len(payload):
            chunk = payload[offset:offset + chunk_size]
            result = client.boot_update_chunk(idx, chunk)
            assert result == 0
            idx    += 1
            offset += len(chunk)

    def test_finalize_after_all_chunks_succeeds(self):
        client, _ = make_client('bootloader')
        pkg = _small_pkg()
        self._do_full_transfer(client, pkg)
        fin = client.boot_update_finalize()
        assert fin['result'] == 0

    def test_finalize_crc_matches_payload(self):
        from tool.src.protocol.crc import crc32_iso_hdlc
        client, _ = make_client('bootloader')
        pkg = _small_pkg()
        payload = pkg[PKG_HEADER_SIZE:]
        self._do_full_transfer(client, pkg)
        fin = client.boot_update_finalize()
        expected = crc32_iso_hdlc(payload)
        assert fin['computed_crc'] == expected

    def test_finalize_without_begin_errors(self):
        client, port = make_client('bootloader')
        frame = encode_frame(PKT_BOOT_UPDATE_FINALIZE, b'', seq=1)
        raw   = port.ft.target.feed(frame)
        resp  = decode_frame(raw)
        assert resp['is_error']


# ── BOOT_UPDATE_ABORT handler ─────────────────────────────────────────────────

class TestBootUpdateAbort:
    def test_abort_resets_state(self):
        client, port = make_client('bootloader')
        pkg = _small_pkg()
        client.boot_update_begin(_pkg_hdr_bytes(pkg))
        client.boot_update_abort()
        # After abort, chunk should fail (state reset)
        frame = encode_frame(
            PKT_BOOT_UPDATE_CHUNK,
            struct.pack('<II', 0, 4) + b'\x00' * 4, seq=1)
        raw  = port.ft.target.feed(frame)
        resp = decode_frame(raw)
        assert resp['is_error']


# ── BootloaderUpdater ─────────────────────────────────────────────────────────

class TestBootloaderUpdater:
    def test_full_update_succeeds(self, tmp_path):
        pkg_path = str(tmp_path / 'fw.pkg')
        pkg      = _small_pkg()
        Path(pkg_path).write_bytes(pkg)

        client, _ = make_client('bootloader')
        updater   = BootloaderUpdater(client)
        result    = updater.update(pkg_path)
        assert result.success
        assert result.chunks_sent > 0
        assert result.computed_crc != 0

    def test_full_update_crc_matches(self, tmp_path):
        from tool.src.protocol.crc import crc32_iso_hdlc
        pkg      = _small_pkg()
        pkg_path = str(tmp_path / 'fw.pkg')
        Path(pkg_path).write_bytes(pkg)
        payload  = pkg[PKG_HEADER_SIZE:]

        client, _ = make_client('bootloader')
        result    = BootloaderUpdater(client).update(pkg_path)
        assert result.computed_crc == crc32_iso_hdlc(payload)

    def test_progress_callback_called(self, tmp_path):
        pkg      = _small_pkg()
        pkg_path = str(tmp_path / 'fw.pkg')
        Path(pkg_path).write_bytes(pkg)

        calls  = []
        client, _ = make_client('bootloader')
        BootloaderUpdater(client).update(pkg_path,
                                         on_progress=lambda d, t: calls.append((d, t)))
        assert calls
        assert calls[-1][0] == calls[-1][1]   # final done == total

    def test_update_missing_file_raises(self):
        client, _ = make_client('bootloader')
        with pytest.raises(UpdateError, match='not found'):
            BootloaderUpdater(client).update('/tmp/no_such_file_bms_12345.pkg')

    def test_update_corrupt_package_raises(self, tmp_path):
        bad = tmp_path / 'bad.pkg'
        bad.write_bytes(bytes(128))
        client, _ = make_client('bootloader')
        with pytest.raises(UpdateError, match='Package invalid|BEGIN rejected'):
            BootloaderUpdater(client).update(str(bad))


# ── TargetModel enter_bootloader + update flow ────────────────────────────────

class TestTargetModelUpdateFlow:
    def test_enter_bootloader_transitions_mode(self):
        from tool.src.core.target_model import TargetModel
        from tool.src.connection.device_state import DeviceMode

        port  = InProcessPort('healthy')
        model = TargetModel(port)
        model.capabilities_handshake()
        assert model.device.mode == DeviceMode.BMS_APP

        model.enter_bootloader()
        model.capabilities_handshake()
        assert model.device.mode == DeviceMode.BOOTLOADER

    def test_enter_bootloader_requires_app_mode(self):
        from tool.src.core.target_model import TargetModel, TargetRefusedError
        from tool.src.connection.device_state import DeviceMode

        port  = InProcessPort('bootloader')
        model = TargetModel(port)
        model.capabilities_handshake()
        assert model.device.mode == DeviceMode.BOOTLOADER

        with pytest.raises(TargetRefusedError):
            model.enter_bootloader()


# ── TCP loopback helpers ──────────────────────────────────────────────────────

def _free_port() -> int:
    with socket.socket() as s:
        s.bind(('127.0.0.1', 0))
        return s.getsockname()[1]


class FakeTcpServer:
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


# ── CLI: update simulate ──────────────────────────────────────────────────────

def test_update_simulate_healthy_target(srv, tmp_path):
    pkg_path = str(tmp_path / 'fw.pkg')
    Path(pkg_path).write_bytes(_small_pkg())
    rc = bmsctl_main(['update', 'simulate', pkg_path] + srv.cli_args())
    assert rc == 0


def test_update_simulate_shows_crc(srv, tmp_path, capsys):
    pkg_path = str(tmp_path / 'fw.pkg')
    Path(pkg_path).write_bytes(_small_pkg())
    bmsctl_main(['update', 'simulate', pkg_path] + srv.cli_args())
    out = capsys.readouterr().out
    assert 'CRC' in out or 'complete' in out


# ── CLI: update validate ──────────────────────────────────────────────────────

def test_update_validate_compatible(srv, tmp_path):
    pkg_path = str(tmp_path / 'fw.pkg')
    Path(pkg_path).write_bytes(_small_pkg())
    rc = bmsctl_main(['update', 'validate', pkg_path] + srv.cli_args())
    assert rc == 0


def test_update_validate_wrong_profile_fails(srv, tmp_path):
    from tool.src.update.package_builder import build_package
    fw = bytes(1024)
    pkg = build_package(fw, hw_profile_id=0x9999)
    pkg_path = str(tmp_path / 'bad.pkg')
    Path(pkg_path).write_bytes(pkg)
    rc = bmsctl_main(['update', 'validate', pkg_path] + srv.cli_args())
    assert rc != 0


# ── CLI: update dry-run ───────────────────────────────────────────────────────

def test_update_dry_run_no_connection_needed(tmp_path, capsys):
    pkg_path = str(tmp_path / 'fw.pkg')
    Path(pkg_path).write_bytes(_small_pkg())
    rc = bmsctl_main(['update', 'dry-run', pkg_path])
    assert rc == 0
    out = capsys.readouterr().out
    assert 'DRY-RUN' in out
    assert 'CHUNK' in out


def test_update_dry_run_missing_file_fails(capsys):
    rc = bmsctl_main(['update', 'dry-run', '/tmp/no_such_file_12345.pkg'])
    assert rc != 0


# ── CLI: config export-json / import-json / export-yaml ─────────────────────

def test_config_export_json(tmp_path):
    json_file = str(tmp_path / 'cfg.json')
    rc = bmsctl_main(['config', 'export-json', '--out', json_file])
    assert rc == 0
    data = json.loads(Path(json_file).read_text())
    assert 'cell_uv_hard_mv' in data


def test_config_export_json_from_bin(tmp_path):
    bin_file  = str(tmp_path / 'cfg.bin')
    json_file = str(tmp_path / 'cfg.json')
    bmsctl_main(['config', 'export-default', '--out', bin_file])
    rc = bmsctl_main(['config', 'export-json', bin_file, '--out', json_file])
    assert rc == 0


def test_config_import_json_round_trip(tmp_path):
    bin_a  = str(tmp_path / 'a.bin')
    json_f = str(tmp_path / 'a.json')
    bin_b  = str(tmp_path / 'b.bin')

    bmsctl_main(['config', 'export-default', '--out', bin_a])
    bmsctl_main(['config', 'export-json',    bin_a, '--out', json_f])
    rc = bmsctl_main(['config', 'import-json', json_f, '--out', bin_b])
    assert rc == 0

    # diff should say identical
    import io
    from contextlib import redirect_stdout
    buf = io.StringIO()
    with redirect_stdout(buf):
        bmsctl_main(['config', 'diff', bin_a, bin_b])
    assert 'identical' in buf.getvalue()


def test_config_export_yaml_skips_without_pyyaml(tmp_path, monkeypatch):
    import sys
    # Hide yaml
    monkeypatch.setitem(sys.modules, 'yaml', None)
    rc = bmsctl_main(['config', 'export-yaml'])
    assert rc != 0
