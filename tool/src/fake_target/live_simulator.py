"""live_simulator.py — Live fake-hardware simulator with evolving state.

Unlike FakeTarget (one instance per TCP connection, static values), LiveFakeHardware:
  - Is ONE shared instance per TCP server.
  - Runs a background tick thread that evolves cell voltages, temperatures, and
    uptime over real time.
  - Is thread-safe: all state access goes through a threading.Lock.
  - Supports 10 live modes (vs 12 static modes in FakeTarget).

Live modes:
  healthy-idle     Cells ±5 mV random drift around 3700 mV; temps ±0.5 °C
  drive            Cells drain 1 mV per 2 ticks from 3700; pack current 50 A
  charge           Cells charge 1 mV per 2 ticks from 3600; pack current -30 A
  cell-uv          cell[0] drifts down to 2400 mV; FAULT_CELL_UV triggers at 2500
  cell-ov          cell[0] drifts up to 4400 mV; FAULT_CELL_OV triggers at 4200
  temp-high        All temps rise 0.1 °C per tick; plateau at 45 °C
  isospi-fault     Static FAULT_ISOSPI_CELL; cells and temps otherwise valid
  openwire-detected  cell[0] open wire; no fault latched (scan result only)
  vpack-invalid    Static FAULT_VPACK_INVALID
  bootloader       Responds as FIRMWARE_TYPE_BOOTLOADER; no ticking
"""
import socket
import threading
import random
import time
from typing import Optional

from .fake_target import FakeTarget, TEMP_INVALID_CX10
from ..protocol.bms_defs import (
    FAULT_BIT_CELL_OV, FAULT_BIT_CELL_UV, FAULT_BIT_ISOSPI_CELL,
    FAULT_BIT_VPACK_INVALID,
)
from ..protocol.framing import FrameDecoder
from ..protocol.packet_defs import TOTAL_CELL_COUNT, TOTAL_TEMP_COUNT, FIRMWARE_TYPE_BOOTLOADER

# NMC OCV table: [(cell_mv, soc_x10), ...] sorted descending by voltage.
_OCV_TABLE = [
    (4200, 1000), (4100, 900), (4000, 800), (3900, 700), (3800, 600),
    (3700, 500),  (3600, 400), (3500, 300), (3400, 200), (3300, 150),
    (3200, 100),  (3100,  50), (3000,   0),
]

def _ocv_to_soc_x10(min_mv: float) -> int:
    if min_mv >= _OCV_TABLE[0][0]:  return 1000
    if min_mv <= _OCV_TABLE[-1][0]: return 0
    for i in range(len(_OCV_TABLE) - 1):
        hi_mv, hi_soc = _OCV_TABLE[i]
        lo_mv, lo_soc = _OCV_TABLE[i + 1]
        if lo_mv <= min_mv <= hi_mv:
            frac = (hi_mv - min_mv) / (hi_mv - lo_mv)
            return int(hi_soc + frac * (lo_soc - hi_soc))
    return 0

LIVE_MODES = frozenset([
    'healthy-idle', 'drive', 'charge',
    'cell-uv', 'cell-ov', 'temp-high',
    'isospi-fault', 'openwire-detected', 'vpack-invalid',
    'bootloader',
])

_DEFAULT_TICK_INTERVAL = 0.2   # seconds


class LiveFakeHardware:
    """Shared, evolving fake target with a background tick thread.

    Each TCP connection gets its own FrameDecoder; they all call feed() on
    the same LiveFakeHardware instance, protected by a lock.
    """

    def __init__(self, mode: str = 'healthy-idle', seed: Optional[int] = None,
                 tick_interval: float = _DEFAULT_TICK_INTERVAL) -> None:
        if mode not in LIVE_MODES:
            raise ValueError(f"Unknown live mode: {mode!r}. Choose from {sorted(LIVE_MODES)}")
        self._mode = mode
        self._lock = threading.Lock()
        self._tick_count = 0
        self._tick_interval = tick_interval
        self._rng = random.Random(seed)
        self._fault_injected: set = set()

        # Initialise the underlying FakeTarget to a clean baseline; _init_mode adjusts it.
        self._target = FakeTarget(mode='healthy')
        self._soc_pct_x10: int = 750   # will be overwritten by _init_mode
        self._init_mode()

        # Start tick thread (daemon so it dies with the process).
        self._running = True
        self._tick_thread = threading.Thread(target=self._tick_loop, daemon=True)
        self._tick_thread.start()

    # ── Mode initialisation ───────────────────────────────────────────────────

    def _init_mode(self) -> None:
        m = self._mode
        # Mode-specific baseline state
        if m == 'healthy-idle':
            self._cell_mv = [3700] * TOTAL_CELL_COUNT
            self._temp_cx10 = [250] * TOTAL_TEMP_COUNT
            self._soc_pct_x10 = _ocv_to_soc_x10(3700)

        elif m == 'drive':
            self._cell_mv = [3700] * TOTAL_CELL_COUNT
            self._temp_cx10 = [280] * TOTAL_TEMP_COUNT
            self._soc_pct_x10 = _ocv_to_soc_x10(3700)
            self._target.set_temps_cx10(self._temp_cx10)

        elif m == 'charge':
            self._cell_mv = [3600] * TOTAL_CELL_COUNT
            self._temp_cx10 = [270] * TOTAL_TEMP_COUNT
            self._target.set_cell_mv(self._cell_mv)
            self._target.set_temps_cx10(self._temp_cx10)
            self._soc_pct_x10 = _ocv_to_soc_x10(3600)

        elif m == 'cell-uv':
            self._cell_mv = [3700] * TOTAL_CELL_COUNT
            self._temp_cx10 = [250] * TOTAL_TEMP_COUNT
            self._soc_pct_x10 = _ocv_to_soc_x10(3700)

        elif m == 'cell-ov':
            self._cell_mv = [3700] * TOTAL_CELL_COUNT
            self._temp_cx10 = [250] * TOTAL_TEMP_COUNT
            self._soc_pct_x10 = _ocv_to_soc_x10(3700)

        elif m == 'temp-high':
            self._cell_mv = [3700] * TOTAL_CELL_COUNT
            self._temp_cx10 = [250] * TOTAL_TEMP_COUNT
            self._soc_pct_x10 = _ocv_to_soc_x10(3700)

        elif m == 'isospi-fault':
            self._target.inject_fault(FAULT_BIT_ISOSPI_CELL)
            self._fault_injected.add('isospi')
            self._cell_mv = [3700] * TOTAL_CELL_COUNT
            self._temp_cx10 = [250] * TOTAL_TEMP_COUNT
            self._soc_pct_x10 = _ocv_to_soc_x10(3700)

        elif m == 'openwire-detected':
            detected = [False] * TOTAL_CELL_COUNT
            detected[0] = True
            self._target.set_open_wire(valid=True, detected=detected)
            self._cell_mv = [3700] * TOTAL_CELL_COUNT
            self._temp_cx10 = [250] * TOTAL_TEMP_COUNT
            self._soc_pct_x10 = _ocv_to_soc_x10(3700)

        elif m == 'vpack-invalid':
            self._target.inject_fault(FAULT_BIT_VPACK_INVALID)
            self._fault_injected.add('vpack')
            self._cell_mv = [3700] * TOTAL_CELL_COUNT
            self._temp_cx10 = [250] * TOTAL_TEMP_COUNT
            self._soc_pct_x10 = _ocv_to_soc_x10(3700)

        elif m == 'bootloader':
            self._target.set_firmware_type(FIRMWARE_TYPE_BOOTLOADER)
            self._cell_mv = [3700] * TOTAL_CELL_COUNT
            self._temp_cx10 = [250] * TOTAL_TEMP_COUNT
            self._soc_pct_x10 = _ocv_to_soc_x10(3700)

        self._target.set_soc(self._soc_pct_x10)

    # ── Tick ──────────────────────────────────────────────────────────────────

    def _tick_loop(self) -> None:
        while self._running:
            time.sleep(self._tick_interval)
            self.tick()

    def tick(self) -> None:
        """Advance simulation by one step. Called by the background thread or tests."""
        with self._lock:
            self._tick_count += 1
            tc = self._tick_count
            m = self._mode
            uptime_ms = tc * int(self._tick_interval * 1000)
            self._target.set_uptime_ms(uptime_ms)

            if m == 'healthy-idle':
                cells = [mv + self._rng.randint(-5, 5) for mv in self._cell_mv]
                temps = [t + self._rng.randint(-3, 3) for t in self._temp_cx10]
                self._target.set_cell_mv(cells)
                self._target.set_temps_cx10(temps)

            elif m == 'drive':
                if tc % 2 == 0:
                    self._cell_mv = [max(2800, mv - 1) for mv in self._cell_mv]
                self._soc_pct_x10 = _ocv_to_soc_x10(min(self._cell_mv))
                self._target.set_cell_mv(self._cell_mv)
                self._target.set_temps_cx10(self._temp_cx10)
                self._target.set_soc(self._soc_pct_x10)

            elif m == 'charge':
                if tc % 2 == 0:
                    self._cell_mv = [min(4100, mv + 1) for mv in self._cell_mv]
                self._soc_pct_x10 = _ocv_to_soc_x10(min(self._cell_mv))
                self._target.set_cell_mv(self._cell_mv)
                self._target.set_temps_cx10(self._temp_cx10)
                self._target.set_soc(self._soc_pct_x10)

            elif m == 'cell-uv':
                if tc % 2 == 0 and self._cell_mv[0] > 2400:
                    self._cell_mv[0] -= 2
                self._target.set_cell_mv(self._cell_mv)
                if self._cell_mv[0] <= 2500 and 'cell_uv' not in self._fault_injected:
                    self._target.inject_fault(FAULT_BIT_CELL_UV)
                    self._fault_injected.add('cell_uv')

            elif m == 'cell-ov':
                if tc % 2 == 0 and self._cell_mv[0] < 4400:
                    self._cell_mv[0] += 2
                self._target.set_cell_mv(self._cell_mv)
                if self._cell_mv[0] >= 4200 and 'cell_ov' not in self._fault_injected:
                    self._target.inject_fault(FAULT_BIT_CELL_OV)
                    self._fault_injected.add('cell_ov')

            elif m == 'temp-high':
                if tc % 2 == 0:
                    self._temp_cx10 = [min(450, t + 1) for t in self._temp_cx10]
                self._target.set_cell_mv(self._cell_mv)
                self._target.set_temps_cx10(self._temp_cx10)

            # Static modes (isospi-fault, openwire-detected, vpack-invalid, bootloader):
            # state was set in _init_mode; only uptime ticks.

    # ── Protocol feed (thread-safe) ───────────────────────────────────────────

    def feed(self, data: bytes, decoder: FrameDecoder) -> bytes:
        """Decode bytes with the per-connection decoder and handle frames under the lock."""
        frames = decoder.feed(data)
        out = b''
        with self._lock:
            for f in frames:
                out += self._target._handle(f)
        return out

    # ── State inspection (for tests) ──────────────────────────────────────────

    @property
    def tick_count(self) -> int:
        return self._tick_count

    def get_cell_mv(self) -> list:
        with self._lock:
            return list(self._target._cell_mv)

    def get_temp_cx10(self) -> list:
        with self._lock:
            return list(self._target._temps_cx10)

    def get_uptime_ms(self) -> int:
        with self._lock:
            return self._target._uptime_ms

    def get_active_faults(self) -> int:
        with self._lock:
            return self._target._active_faults

    def stop(self) -> None:
        """Stop the background tick thread."""
        self._running = False

    # ── TCP server ────────────────────────────────────────────────────────────

    @classmethod
    def serve_tcp(cls, host: str = '127.0.0.1', port: int = 65102,
                  mode: str = 'healthy-idle', seed: Optional[int] = None) -> None:
        """Block and serve connections.  One shared LiveFakeHardware instance per server."""
        instance = cls(mode=mode, seed=seed)
        server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        server.bind((host, port))
        server.listen(5)
        print(f"[live_simulator] listening on {host}:{port}  mode={mode}  seed={seed}",
              flush=True)
        while True:
            conn, addr = server.accept()
            print(f"[live_simulator] client {addr}", flush=True)
            t = threading.Thread(target=cls._handle_client,
                                 args=(conn, instance), daemon=True)
            t.start()

    @classmethod
    def _handle_client(cls, conn: socket.socket, instance: 'LiveFakeHardware') -> None:
        decoder = FrameDecoder()
        try:
            while True:
                data = conn.recv(4096)
                if not data:
                    break
                resp = instance.feed(data, decoder)
                if resp:
                    conn.sendall(resp)
        except Exception:
            pass
        finally:
            conn.close()


# ── CLI entry point ───────────────────────────────────────────────────────────

if __name__ == '__main__':
    import argparse
    parser = argparse.ArgumentParser(
        description='BMS live fake-hardware simulator (evolving state).')
    parser.add_argument('--mode', default='healthy-idle',
                        choices=sorted(LIVE_MODES),
                        help='Simulation mode (default: healthy-idle)')
    parser.add_argument('--host', default='127.0.0.1',
                        help='TCP bind address (default: 127.0.0.1)')
    parser.add_argument('--port', type=int, default=65103,
                        help='TCP port (default: 65103)')
    parser.add_argument('--seed', type=int, default=None,
                        help='Random seed for deterministic drift (default: random)')
    args = parser.parse_args()
    LiveFakeHardware.serve_tcp(host=args.host, port=args.port,
                               mode=args.mode, seed=args.seed)
