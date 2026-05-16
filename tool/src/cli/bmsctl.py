#!/usr/bin/env python3
"""bmsctl.py — BMS command-line tool.

Thin wrapper around the tool backend.  The GUI is the primary operator interface;
this CLI exists for developer use and automated testing.

Usage:
    python -m tool.src.cli.bmsctl <command> [options]
    python -m tool.src.cli.bmsctl --help
"""
import argparse
import json
import struct
import sys
from pathlib import Path
from typing import Optional

# Make importable when run as a script from repo root
sys.path.insert(0, str(Path(__file__).resolve().parents[4]))

from tool.src.core.connection_manager import ConnectionManager
from tool.src.core.target_model import TargetModel, TargetRefusedError
from tool.src.protocol.client import ProtocolError
from tool.src.connection.device_state import DeviceMode
from tool.src.config.schema import BmsConfig
from tool.src.config.validator import validate_config
from tool.src.update.package_builder import build_package, PackageBuildError
from tool.src.update.package_parser import parse_and_validate_package, PackageValidationError
from tool.src.update.stlink import dry_run_app, detect_programmer

# ── Fault name table (matches protocol/fault_bits.yaml) ──────────────────────

_FAULT_NAMES = [
    "CELL_OV", "CELL_UV", "CELL_OV_SOFT", "CELL_UV_SOFT",
    "CELL_READ_INVALID", "CELL_OPENWIRE", "TEMP_OVER_CHARGE", "TEMP_OVER_DISCHARGE",
    "TEMP_OVER_ABS", "TEMP_READ_INVALID", "TEMP_COVERAGE", "VBAT_INVALID",
    "VPACK_INVALID", "PRECHARGE_TIMEOUT", "PRECHARGE_DELTA", "ISOSPI_CELL",
    "ISOSPI_TEMP", "I2C_ISL28022", "WATCHDOG", "CONFIG_INVALID",
    "OVERCURRENT", "BALANCE_TEMP_VIOLATION", "TEMP_CHAIN_BALANCE_ATTEMPT",
    "TEMP_COLD_CHARGE", "TEMP_COLD_DISCHARGE",
]

# ── Helpers ───────────────────────────────────────────────────────────────────

def _out(data, as_json: bool) -> None:
    if as_json:
        print(json.dumps(data, indent=2, default=str))
    elif isinstance(data, dict):
        for k, v in data.items():
            if isinstance(v, bytes):
                v = v.hex()
            elif isinstance(v, list) and len(v) > 20:
                v = f"[{len(v)} items]"
            print(f"  {k}: {v}")
    else:
        print(data)


def _connect(args) -> tuple:
    """Return (ConnectionManager, TargetModel) after handshake or exit(1)."""
    mgr = ConnectionManager()
    try:
        if getattr(args, 'serial', None):
            port = mgr.connect_serial(args.serial, getattr(args, 'baud', 115200))
        else:
            port = mgr.connect_tcp(
                getattr(args, 'host', '127.0.0.1'),
                getattr(args, 'port', 65102))
    except (OSError, IOError) as e:
        print(f"error: cannot connect — {e}", file=sys.stderr)
        sys.exit(1)

    model  = TargetModel(port)
    device = model.capabilities_handshake()
    if device.mode == DeviceMode.DISCONNECTED:
        print(f"error: {device.error_msg}", file=sys.stderr)
        mgr.disconnect()
        sys.exit(1)
    return mgr, model


def _add_connect_args(p: argparse.ArgumentParser) -> None:
    p.add_argument('--host',   default='127.0.0.1')
    p.add_argument('--port',   type=int, default=65102)
    p.add_argument('--serial', default=None, help='Serial device (e.g. /dev/ttyUSB0)')
    p.add_argument('--baud',   type=int, default=115200)
    p.add_argument('--json',   action='store_true')


def _print_fault_bits(mask: int, label: str) -> None:
    for bit in range(64):
        if mask & (1 << bit):
            name = _FAULT_NAMES[bit] if bit < len(_FAULT_NAMES) else f"BIT_{bit}"
            print(f"    [{label}] bit {bit:2d}: {name}")


# ── Sub-command implementations ───────────────────────────────────────────────

def cmd_fake_target_run(args) -> int:
    import socket as sock_mod
    import threading
    from tool.src.fake_target.fake_target import FakeTarget

    host, tcp_port = ('127.0.0.1', 65102)
    if ':' in str(args.bind):
        h, p = args.bind.rsplit(':', 1)
        host, tcp_port = h, int(p)
    else:
        tcp_port = int(args.bind)

    server = sock_mod.socket(sock_mod.AF_INET, sock_mod.SOCK_STREAM)
    server.setsockopt(sock_mod.SOL_SOCKET, sock_mod.SO_REUSEADDR, 1)
    server.bind((host, tcp_port))
    server.listen(5)
    print(f"[fake_target] listening on {host}:{tcp_port}  mode={args.mode}", flush=True)

    mode = args.mode
    while True:
        conn, addr = server.accept()
        print(f"[fake_target] client connected: {addr}", flush=True)
        t = threading.Thread(target=_serve_client, args=(conn, mode), daemon=True)
        t.start()


def _serve_client(conn, mode: str) -> None:
    from tool.src.fake_target.fake_target import FakeTarget
    target = FakeTarget(mode=mode)
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


def cmd_fake_target_self_test(args) -> int:
    from tool.src.fake_target.fake_target import FakeTargetInProcess
    from tool.src.protocol.framing import encode_frame, decode_frame
    from tool.src.protocol.packet_defs import PKT_GET_CAPABILITIES, PKT_GET_FAULTS

    passed = failed = 0
    modes = ['healthy', 'cell_uv', 'cell_ov', 'temp_invalid',
             'isospi_fault', 'config_error', 'vpack_invalid',
             'precharge_fault', 'bootloader', 'safe_invalid']

    for mode in modes:
        try:
            ft    = FakeTargetInProcess(mode=mode)
            frame = encode_frame(PKT_GET_CAPABILITIES, b'', seq=1)
            resp  = ft.exchange(frame)
            d     = decode_frame(resp)
            assert not d['is_error'], f"capabilities returned error for mode={mode}"
            passed += 1
            print(f"  PASS  mode={mode}")
        except Exception as e:
            failed += 1
            print(f"  FAIL  mode={mode}: {e}")

    # fault injection round-trip
    try:
        ft = FakeTargetInProcess()
        ft.inject_fault(0)
        frame = encode_frame(PKT_GET_FAULTS, b'', seq=1)
        resp  = ft.exchange(frame)
        d     = decode_frame(resp)
        active = struct.unpack_from('<Q', d['payload'], 0)[0]
        assert active & 1, "FAULT_CELL_OV not reflected after inject_fault(0)"
        passed += 1
        print("  PASS  fault_injection_roundtrip")
    except Exception as e:
        failed += 1
        print(f"  FAIL  fault_injection_roundtrip: {e}")

    print(f"\nfake-target self-test: {passed} passed, {failed} failed")
    return 0 if failed == 0 else 1


def cmd_connect(args) -> int:
    mgr, model = _connect(args)
    d    = model.device
    caps = d.capabilities
    data = {
        'mode':                  d.mode.name,
        'firmware_version':      '.'.join(str(x) for x in caps.firmware_version) if caps else None,
        'hw_profile_id':         f"0x{caps.hw_profile_id:04X}" if caps else None,
        'protocol_version':      caps.protocol_version if caps else None,
        'config_schema_version': caps.config_schema_version if caps else None,
        'cell_count':            caps.cell_count if caps else None,
        'feature_flags':         f"0x{caps.feature_flags:08X}" if caps else None,
    }
    _out({k: v for k, v in data.items() if v is not None}, args.json)
    mgr.disconnect()
    return 0


def cmd_values(args) -> int:
    mgr, model = _connect(args)
    try:
        vs = model.poll_values()
        _out({
            'vbat_mv':        vs.vbat_mv,
            'vpack_mv':       vs.vpack_mv,
            'i_batt_ma':      vs.i_batt_ma,
            'bms_state':      vs.bms_state,
            'active_faults':  f"0x{vs.active_faults:016X}",
            'latched_faults': f"0x{vs.latched_faults:016X}",
            'outputs_state':  f"0x{vs.outputs_state:02X}",
            'uptime_ms':      vs.uptime_ms,
        }, args.json)
        return 0
    except (ProtocolError, TargetRefusedError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    finally:
        mgr.disconnect()


def cmd_cells(args) -> int:
    mgr, model = _connect(args)
    try:
        cs = model.poll_cells()
        mv = cs.cells_mv
        if args.json:
            _out({'cell_count': cs.cell_count, 'cells_mv': mv,
                  'validity': cs.validity, 'timestamp_ms': cs.timestamp_ms}, True)
        else:
            print(f"  cell_count:  {cs.cell_count}")
            if mv:
                print(f"  min_mv:      {min(mv)}")
                print(f"  max_mv:      {max(mv)}")
                print(f"  avg_mv:      {sum(mv)//len(mv)}")
                print(f"  mismatch_mv: {max(mv)-min(mv)}")
            if args.verbose:
                for i, v in enumerate(mv):
                    tag = '' if (cs.validity is None or cs.validity[i]) else '  [INVALID]'
                    print(f"  cell[{i:02d}]: {v} mV{tag}")
        return 0
    except (ProtocolError, TargetRefusedError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    finally:
        mgr.disconnect()


def cmd_temps(args) -> int:
    mgr, model = _connect(args)
    try:
        ts = model.poll_temps()
        if args.json:
            _out({'temp_count': ts.temp_count, 'temps_cx10': ts.temps_cx10}, True)
        else:
            valid = [t for t in ts.temps_cx10 if t != -0x8000]
            inv   = ts.temp_count - len(valid)
            print(f"  temp_count: {ts.temp_count}")
            if valid:
                print(f"  max: {max(valid)/10:.1f}°C")
                print(f"  avg: {sum(valid)/len(valid)/10:.1f}°C")
                print(f"  min: {min(valid)/10:.1f}°C")
            if inv:
                print(f"  invalid: {inv}")
            if args.verbose:
                for i, t in enumerate(ts.temps_cx10):
                    print(f"  temp[{i:02d}]: {'INVALID' if t == -0x8000 else f'{t/10:.1f}°C'}")
        return 0
    except (ProtocolError, TargetRefusedError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    finally:
        mgr.disconnect()


def cmd_faults(args) -> int:
    mgr, model = _connect(args)
    try:
        fs = model.poll_faults()
        if args.json:
            _out({'active_faults': fs.active_faults,
                  'latched_faults': fs.latched_faults}, True)
        else:
            print(f"  active_faults:  0x{fs.active_faults:016X}")
            print(f"  latched_faults: 0x{fs.latched_faults:016X}")
            _print_fault_bits(fs.active_faults,  "ACTIVE")
            _print_fault_bits(fs.latched_faults, "LATCHED")
        return 0
    except (ProtocolError, TargetRefusedError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    finally:
        mgr.disconnect()


def cmd_diagnostics(args) -> int:
    mgr, model = _connect(args)
    try:
        ds = model.poll_diagnostics()
        _out({
            'reset_cause':     f"0x{ds.reset_cause:02X}",
            'pec_cell_errors': ds.pec_cell_errors,
            'pec_temp_errors': ds.pec_temp_errors,
            'i2c_errors':      ds.i2c_errors,
            'open_wire_valid': ds.open_wire_valid,
            'open_wire_mask':  ds.open_wire_mask.hex(),
            'uptime_ms':       ds.uptime_ms,
        }, args.json)
        return 0
    except (ProtocolError, TargetRefusedError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    finally:
        mgr.disconnect()


def cmd_config_export_default(args) -> int:
    cfg  = BmsConfig()
    blob = cfg.pack()
    if getattr(args, 'out', None):
        Path(args.out).write_bytes(blob)
        print(f"Written {len(blob)} bytes to {args.out}")
    else:
        sys.stdout.buffer.write(blob)
    return 0


def cmd_config_validate(args) -> int:
    try:
        blob = Path(args.file).read_bytes()
    except FileNotFoundError:
        print(f"error: file not found: {args.file}", file=sys.stderr)
        return 1
    cfg = BmsConfig.unpack(blob)
    ok, err_off, msg = validate_config(cfg)
    _out({'ok': ok, 'err_offset': err_off if not ok else None, 'message': msg},
         getattr(args, 'json', False))
    return 0 if ok else 1


def cmd_config_read(args) -> int:
    mgr, model = _connect(args)
    try:
        cfg  = model.read_config()
        blob = cfg.pack()
        if getattr(args, 'out', None):
            Path(args.out).write_bytes(blob)
            print(f"Written {len(blob)} bytes to {args.out}")
        else:
            print(f"  config read: {len(blob)} bytes  generation={cfg.config_generation}")
        return 0
    except (ProtocolError, TargetRefusedError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    finally:
        mgr.disconnect()


def cmd_config_apply_ram(args) -> int:
    try:
        blob = Path(args.file).read_bytes()
    except FileNotFoundError:
        print(f"error: file not found: {args.file}", file=sys.stderr)
        return 1
    cfg = BmsConfig.unpack(blob)
    mgr, model = _connect(args)
    try:
        ok, err_off, msg = model.apply_config_ram(cfg)
        _out({'ok': ok, 'err_offset': err_off, 'message': msg}, args.json)
        return 0 if ok else 1
    except (ProtocolError, TargetRefusedError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    finally:
        mgr.disconnect()


def cmd_config_diff(args) -> int:
    try:
        cfg_a = BmsConfig.unpack(Path(args.a).read_bytes())
        cfg_b = BmsConfig.unpack(Path(args.b).read_bytes())
    except FileNotFoundError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    diffs = [(f, getattr(cfg_a, f), getattr(cfg_b, f))
             for f in cfg_a.__dataclass_fields__
             if getattr(cfg_a, f) != getattr(cfg_b, f)]
    if not diffs:
        print("  configs are identical")
    else:
        for name, va, vb in diffs:
            print(f"  {name}: {va!r}  →  {vb!r}")
    return 0


def cmd_package_build(args) -> int:
    try:
        fw = Path(args.input).read_bytes()
    except FileNotFoundError:
        print(f"error: input not found: {args.input}", file=sys.stderr)
        return 1
    try:
        major, minor, patch = (int(x) for x in args.version.split('.'))
    except ValueError:
        print(f"error: bad version string: {args.version}", file=sys.stderr)
        return 1
    try:
        pkg = build_package(fw, fw_version=(major, minor, patch))
    except PackageBuildError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    Path(args.output).write_bytes(pkg)
    print(f"Written {len(pkg)} bytes to {args.output}")
    return 0


def cmd_package_inspect(args) -> int:
    try:
        from tool.src.update.package_parser import parse_header
        from tool.src.update.package_builder import PKG_HEADER_SIZE
        raw = Path(args.file).read_bytes()
        hdr = parse_header(raw[:PKG_HEADER_SIZE])
        _out({
            'magic':          f"0x{hdr.pkg_magic:08X}",
            'fw_version':     '.'.join(str(x) for x in hdr.fw_version),
            'hw_profile_id':  f"0x{hdr.hw_profile_id:04X}",
            'app_start_addr': f"0x{hdr.app_start_addr:08X}",
            'app_size':       hdr.app_size,
            'app_crc32':      f"0x{hdr.app_crc32:08X}",
            'header_crc32':   f"0x{hdr.pkg_header_crc32:08X}",
        }, getattr(args, 'json', False))
        return 0
    except FileNotFoundError:
        print(f"error: not found: {args.file}", file=sys.stderr)
        return 1
    except Exception as e:
        print(f"error: {e}", file=sys.stderr)
        return 1


def cmd_package_validate(args) -> int:
    try:
        hdr, payload = parse_and_validate_package(args.file)
        _out({'valid': True,
              'fw_version': '.'.join(str(x) for x in hdr.fw_version),
              'app_size': hdr.app_size}, getattr(args, 'json', False))
        return 0
    except PackageValidationError as e:
        _out({'valid': False, 'error': str(e)}, getattr(args, 'json', False))
        return 1
    except FileNotFoundError:
        print(f"error: not found: {args.file}", file=sys.stderr)
        return 1


def cmd_stlink_dry_run(args) -> int:
    try:
        _cmd, status = dry_run_app(args.file)
        print(status)
        return 0
    except (FileNotFoundError, PackageValidationError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 1


# ── Argument parser ───────────────────────────────────────────────────────────

def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog='bmsctl',
        description='BMS command-line tool — thin wrapper around the backend',
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    sub = parser.add_subparsers(dest='command')

    # ── fake-target ──────────────────────────────────────────────────────────
    ft     = sub.add_parser('fake-target', help='Fake target utilities')
    ft_sub = ft.add_subparsers(dest='ft_command')

    ft_run = ft_sub.add_parser('run', help='Run fake TCP target server (Ctrl-C to stop)')
    ft_run.add_argument('--mode', default='healthy',
                        choices=['healthy', 'cell_uv', 'cell_ov', 'temp_invalid',
                                 'isospi_fault', 'config_error', 'vpack_invalid',
                                 'precharge_fault', 'bootloader', 'safe_invalid'])
    ft_run.add_argument('--bind', default='127.0.0.1:65102', metavar='HOST:PORT')

    ft_sub.add_parser('self-test', help='Run fake target self-test suite and exit')

    # ── connect ──────────────────────────────────────────────────────────────
    p = sub.add_parser('connect', help='Connect and show capabilities')
    _add_connect_args(p)

    # ── values / cells / temps / faults / diagnostics ────────────────────────
    for name in ('values', 'faults', 'diagnostics'):
        p = sub.add_parser(name, help=f'Read {name}')
        _add_connect_args(p)

    for name in ('cells', 'temps'):
        p = sub.add_parser(name, help=f'Read {name}')
        _add_connect_args(p)
        p.add_argument('-v', '--verbose', action='store_true')

    # ── config ───────────────────────────────────────────────────────────────
    cfg     = sub.add_parser('config', help='Configuration operations')
    cfg_sub = cfg.add_subparsers(dest='cfg_command')

    p = cfg_sub.add_parser('export-default', help='Export default config blob')
    p.add_argument('--out', default=None, metavar='FILE')
    p.add_argument('--json', action='store_true')

    p = cfg_sub.add_parser('validate', help='Validate config file offline')
    p.add_argument('file')
    p.add_argument('--json', action='store_true')

    p = cfg_sub.add_parser('read', help='Read config from connected target')
    _add_connect_args(p)
    p.add_argument('--out', default=None, metavar='FILE')

    p = cfg_sub.add_parser('apply-ram', help='Apply config to target RAM (no flash write)')
    _add_connect_args(p)
    p.add_argument('file')

    p = cfg_sub.add_parser('diff', help='Diff two config files')
    p.add_argument('a')
    p.add_argument('b')

    # ── package ──────────────────────────────────────────────────────────────
    pkg     = sub.add_parser('package', help='Firmware package operations')
    pkg_sub = pkg.add_subparsers(dest='pkg_command')

    p = pkg_sub.add_parser('build', help='Build .pkg from firmware .bin')
    p.add_argument('input')
    p.add_argument('output')
    p.add_argument('--version', default='0.1.0', metavar='MAJOR.MINOR.PATCH')

    p = pkg_sub.add_parser('inspect', help='Inspect .pkg header')
    p.add_argument('file')
    p.add_argument('--json', action='store_true')

    p = pkg_sub.add_parser('validate', help='Validate .pkg file fully')
    p.add_argument('file')
    p.add_argument('--json', action='store_true')

    # ── stlink ───────────────────────────────────────────────────────────────
    sl     = sub.add_parser('stlink', help='ST-Link flash operations')
    sl_sub = sl.add_subparsers(dest='sl_command')

    p = sl_sub.add_parser('dry-run-app', help='Show flash command without executing')
    p.add_argument('file', help='.pkg file')

    return parser


def main(argv=None) -> int:
    parser = build_parser()
    args   = parser.parse_args(argv)

    if not args.command:
        parser.print_help()
        return 1

    if args.command == 'fake-target':
        if not getattr(args, 'ft_command', None):
            parser.parse_args(['fake-target', '--help'])
            return 1
        if args.ft_command == 'run':       return cmd_fake_target_run(args)
        if args.ft_command == 'self-test': return cmd_fake_target_self_test(args)

    elif args.command == 'connect':     return cmd_connect(args)
    elif args.command == 'values':      return cmd_values(args)
    elif args.command == 'cells':       return cmd_cells(args)
    elif args.command == 'temps':       return cmd_temps(args)
    elif args.command == 'faults':      return cmd_faults(args)
    elif args.command == 'diagnostics': return cmd_diagnostics(args)

    elif args.command == 'config':
        cc = getattr(args, 'cfg_command', None)
        if not cc:
            return 1
        if cc == 'export-default': return cmd_config_export_default(args)
        if cc == 'validate':       return cmd_config_validate(args)
        if cc == 'read':           return cmd_config_read(args)
        if cc == 'apply-ram':      return cmd_config_apply_ram(args)
        if cc == 'diff':           return cmd_config_diff(args)

    elif args.command == 'package':
        pc = getattr(args, 'pkg_command', None)
        if not pc:
            return 1
        if pc == 'build':    return cmd_package_build(args)
        if pc == 'inspect':  return cmd_package_inspect(args)
        if pc == 'validate': return cmd_package_validate(args)

    elif args.command == 'stlink':
        sc = getattr(args, 'sl_command', None)
        if not sc:
            return 1
        if sc == 'dry-run-app': return cmd_stlink_dry_run(args)

    parser.print_help()
    return 1


if __name__ == '__main__':
    sys.exit(main())
