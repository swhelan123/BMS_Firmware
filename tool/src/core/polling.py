"""polling.py — background polling loop that periodically updates AppState."""
import threading
from typing import Optional

from ..protocol.client import ProtocolError
from ..protocol.bms_defs import BMS_STATE_CHARGE
from .target_model import TargetModel, TargetRefusedError
from .app_state import AppState
from .logging_model import EventLog


class PollingLoop:
    """Background thread that polls a TargetModel and writes results into AppState.

    Polling order per cycle: values → cells → temps → faults.
    Each sub-poll is independent; a single failure doesn't abort the cycle.
    """

    DEFAULT_INTERVAL = 0.5  # seconds

    def __init__(self, model: TargetModel, state: AppState,
                 event_log: Optional[EventLog] = None,
                 interval: float = DEFAULT_INTERVAL):
        self._model      = model
        self._state      = state
        self._event_log  = event_log
        self._interval   = interval
        self._thread:    Optional[threading.Thread] = None
        self._stop_evt   = threading.Event()

    def start(self) -> None:
        if self._thread and self._thread.is_alive():
            return
        self._stop_evt.clear()
        self._thread = threading.Thread(target=self._loop, daemon=True,
                                        name='bms-polling')
        self._thread.start()

    def stop(self, timeout: float = 2.0) -> None:
        self._stop_evt.set()
        if self._thread:
            self._thread.join(timeout=timeout)
            self._thread = None

    @property
    def running(self) -> bool:
        return bool(self._thread and self._thread.is_alive())

    def _log(self, msg: str) -> None:
        if self._event_log:
            self._event_log.append(msg)

    def _try_poll(self, name: str, fn, update_fn) -> None:
        try:
            result = fn()
            update_fn(result)
        except (ProtocolError, TargetRefusedError, IOError) as e:
            self._log(f"poll_{name}: {e}")

    def _poll_once(self) -> None:
        self._try_poll('values',      self._model.poll_values,      self._state.update_values)
        if self._stop_evt.is_set():
            return
        self._try_poll('cells',       self._model.poll_cells,       self._state.update_cells)
        if self._stop_evt.is_set():
            return
        self._try_poll('temps',       self._model.poll_temps,       self._state.update_temps)
        if self._stop_evt.is_set():
            return
        self._try_poll('faults',      self._model.poll_faults,      self._state.update_faults)
        if self._stop_evt.is_set():
            return
        # Charger status only means anything mid-charge — the CAN link is in
        # drive mode (500 kbit/s, no RX consumed) the rest of the time, so
        # polling it then would just repeatedly report status_valid=False.
        if self._state.values.bms_state == BMS_STATE_CHARGE:
            self._try_poll('charger_status', self._model.poll_charger_status,
                           self._state.update_charger)

    def _loop(self) -> None:
        while not self._stop_evt.is_set():
            try:
                self._poll_once()
            except Exception as e:
                self._log(f"polling loop unhandled error: {e}")
            self._stop_evt.wait(timeout=self._interval)
