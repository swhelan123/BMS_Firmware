"""app_state.py — central application state shared between polling loop and GUI."""
from dataclasses import dataclass, field
from typing import Optional, List, Callable
import threading

from ..connection.device_state import DeviceState


@dataclass
class ValuesState:
    vbat_mv:           int  = 0
    vpack_mv:          int  = 0
    i_batt_ma:         int  = 0
    bms_state:         int  = 0
    active_faults:     int  = 0
    latched_faults:    int  = 0
    outputs_state:     int  = 0
    uptime_ms:         int  = 0
    measurement_flags: int  = 0
    soc_pct_x10:       int  = -1   # 0–1000 = 0.0%–100.0%, -1 = unknown
    valid:             bool = False


@dataclass
class CellsState:
    cell_count:   int              = 0
    cells_mv:     List[int]        = field(default_factory=list)
    validity:     Optional[List[bool]] = None
    timestamp_ms: int              = 0
    valid:        bool             = False


@dataclass
class TempsState:
    temp_count: int       = 0
    temps_cx10: List[int] = field(default_factory=list)
    raw_mv:     List[int] = field(default_factory=list)  # raw C-input mV per channel
    valid:      bool      = False


@dataclass
class FaultsState:
    active_faults:  int  = 0
    latched_faults: int  = 0
    valid:          bool = False


@dataclass
class ChargerStatusState:
    status_valid:          bool = False
    output_voltage_dv:     int  = 0
    output_current_da:     int  = 0
    status_flags:          int  = 0
    termination_requested: bool = False
    status_age_ms:         int  = 0
    valid:                 bool = False  # protocol round-trip itself succeeded


@dataclass
class DiagnosticsState:
    reset_cause:     int   = 0
    pec_cell_errors: int   = 0
    pec_temp_errors: int   = 0
    i2c_errors:      int   = 0
    open_wire_valid: bool  = False
    open_wire_mask:  bytes = field(default_factory=lambda: bytes(10))
    uptime_ms:       int   = 0
    valid:           bool  = False


class AppState:
    """Central application state.  Thread-safe; listener callbacks run on the caller's thread."""

    def __init__(self):
        self._lock       = threading.Lock()
        self.device      = DeviceState()
        self.values      = ValuesState()
        self.cells       = CellsState()
        self.temps       = TempsState()
        self.faults      = FaultsState()
        self.diagnostics = DiagnosticsState()
        self.charger     = ChargerStatusState()
        self._listeners: List[Callable[[str], None]] = []

    # ── Observer ──────────────────────────────────────────────────────────────

    def subscribe(self, fn: Callable[[str], None]) -> None:
        with self._lock:
            self._listeners.append(fn)

    def unsubscribe(self, fn: Callable[[str], None]) -> None:
        with self._lock:
            self._listeners = [l for l in self._listeners if l is not fn]

    def _notify(self, key: str) -> None:
        with self._lock:
            listeners = list(self._listeners)
        for fn in listeners:
            try:
                fn(key)
            except Exception:
                pass

    # ── Updaters ──────────────────────────────────────────────────────────────

    def update_device(self, s: DeviceState) -> None:
        with self._lock:
            self.device = s
        self._notify('device')

    def update_values(self, s: ValuesState) -> None:
        with self._lock:
            self.values = s
        self._notify('values')

    def update_cells(self, s: CellsState) -> None:
        with self._lock:
            self.cells = s
        self._notify('cells')

    def update_temps(self, s: TempsState) -> None:
        with self._lock:
            self.temps = s
        self._notify('temps')

    def update_faults(self, s: FaultsState) -> None:
        with self._lock:
            self.faults = s
        self._notify('faults')

    def update_diagnostics(self, s: DiagnosticsState) -> None:
        with self._lock:
            self.diagnostics = s
        self._notify('diagnostics')

    def update_charger(self, s: ChargerStatusState) -> None:
        with self._lock:
            self.charger = s
        self._notify('charger')

    def reset(self) -> None:
        with self._lock:
            self.device      = DeviceState()
            self.values      = ValuesState()
            self.cells       = CellsState()
            self.temps       = TempsState()
            self.faults      = FaultsState()
            self.diagnostics = DiagnosticsState()
            self.charger     = ChargerStatusState()
        self._notify('reset')
