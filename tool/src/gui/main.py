"""main.py — BMS desktop tool GUI entry point.

Install dependency:
    pip install PyQt6

Run:
    python -m tool.src.gui.main
    python -m tool.src.gui.main --fake --mode healthy
"""
import sys
import threading
from typing import Optional

from PyQt6.QtWidgets import (
    QApplication, QMainWindow, QTabWidget, QWidget,
    QStatusBar, QLabel, QVBoxLayout,
)
from PyQt6.QtCore import Qt, QTimer, pyqtSignal, QObject
from PyQt6.QtGui import QIcon

from ..core.app_state import AppState
from ..core.connection_manager import ConnectionManager
from ..core.target_model import TargetModel
from ..core.polling import PollingLoop
from ..core.logging_model import EventLog, PacketLog
from ..connection.device_state import DeviceState, DeviceMode

from .pages.connection import ConnectionPage
from .pages.dashboard import DashboardPage
from .pages.cells import CellsPage
from .pages.temperatures import TemperaturesPage
from .pages.faults import FaultsPage
from .pages.config import ConfigPage
from .pages.firmware_flash import FirmwareFlashPage
from .pages.logs import LogsPage


class _StateSignals(QObject):
    """Signals emitted on the polling thread; Qt delivers them to the main thread."""
    updated = pyqtSignal(str)  # key: 'device', 'values', 'cells', etc.


class BmsMainWindow(QMainWindow):
    def __init__(self):
        super().__init__()
        self.setWindowTitle("BMS Tool")
        self.resize(1100, 750)

        self._state    = AppState()
        self._conn_mgr = ConnectionManager()
        self._model:   Optional[TargetModel] = None
        self._polling: Optional[PollingLoop] = None
        self._evt_log  = EventLog()
        self._pkt_log  = PacketLog()
        self._signals  = _StateSignals()
        self._signals.updated.connect(self._on_state_updated)
        self._state.subscribe(lambda key: self._signals.updated.emit(key))

        self._build_ui()

    # ── UI construction ───────────────────────────────────────────────────────

    def _build_ui(self) -> None:
        tabs = QTabWidget()
        self._tabs = tabs

        self._page_conn  = ConnectionPage(self._state, self)
        self._page_dash  = DashboardPage(self._state)
        self._page_cells = CellsPage(self._state)
        self._page_temps = TemperaturesPage(self._state)
        self._page_fault = FaultsPage(self._state, self)
        self._page_cfg   = ConfigPage(self._state, self)
        self._page_flash = FirmwareFlashPage(self._state, self)
        self._page_logs  = LogsPage(self._evt_log, self._pkt_log)

        tabs.addTab(self._page_conn,  "Connection")
        tabs.addTab(self._page_dash,  "Dashboard")
        tabs.addTab(self._page_cells, "Cells")
        tabs.addTab(self._page_temps, "Temperatures")
        tabs.addTab(self._page_fault, "Faults")
        tabs.addTab(self._page_cfg,   "Config")
        tabs.addTab(self._page_flash, "Firmware Flash")
        tabs.addTab(self._page_logs,  "Logs")

        self.setCentralWidget(tabs)

        bar = self.statusBar()
        self._status_label = QLabel("Disconnected")
        bar.addWidget(self._status_label)

        self._page_conn.connect_requested.connect(self._on_connect_requested)
        self._page_conn.disconnect_requested.connect(self._on_disconnect)
        self._page_fault.clear_latched_requested.connect(self._on_clear_latched)

    # ── Slots ─────────────────────────────────────────────────────────────────

    def _on_connect_requested(self, host: str, port: int) -> None:
        self._evt_log.append(f"Connecting to {host}:{port} …")
        try:
            transport = self._conn_mgr.connect_tcp(host, port)
        except (OSError, IOError) as e:
            self._evt_log.append(f"Connection failed: {e}")
            self._status_label.setText(f"Error: {e}")
            return

        self._model  = TargetModel(transport)
        device       = self._model.capabilities_handshake()
        self._state.update_device(device)
        self._evt_log.append(f"Connected: mode={device.mode.name}")

        if device.mode == DeviceMode.BMS_APP:
            self._polling = PollingLoop(self._model, self._state, self._evt_log)
            self._polling.start()

    def _on_disconnect(self) -> None:
        if self._polling:
            self._polling.stop()
            self._polling = None
        self._conn_mgr.disconnect()
        self._model = None
        self._state.reset()
        self._evt_log.append("Disconnected.")

    def _on_clear_latched(self, mask: int) -> None:
        if self._model is None:
            return
        try:
            cleared = self._model.clear_latched_faults(mask)
            self._evt_log.append(f"Cleared latched faults: 0x{cleared:016X}")
        except Exception as e:
            self._evt_log.append(f"clear_latched_faults error: {e}")

    def _on_state_updated(self, key: str) -> None:
        """Called on the main thread whenever AppState changes."""
        device = self._state.device
        if key == 'device':
            mode_txt = device.mode.name
            err = f" — {device.error_msg}" if device.error_msg else ""
            self._status_label.setText(f"{mode_txt}{err}")

        self._page_dash.refresh(self._state)
        self._page_cells.refresh(self._state)
        self._page_temps.refresh(self._state)
        self._page_fault.refresh(self._state)
        self._page_cfg.refresh(self._state)
        self._page_flash.refresh(self._state)
        self._page_logs.refresh()

        # Enable/disable runtime tabs based on mode
        is_app = (device.mode == DeviceMode.BMS_APP)
        for idx in range(1, self._tabs.count() - 1):  # skip Connection and Logs
            self._tabs.setTabEnabled(idx, is_app)

    def closeEvent(self, event):
        self._on_disconnect()
        super().closeEvent(event)


def main(argv=None) -> int:
    import argparse
    parser = argparse.ArgumentParser(prog='bms-gui')
    parser.add_argument('--fake', action='store_true',
                        help='Auto-connect to fake TCP target on localhost:65102')
    parser.add_argument('--mode', default='healthy',
                        help='Fake target mode when --fake is used')
    args, qt_args = parser.parse_known_args(argv)

    app = QApplication(sys.argv[:1] + qt_args)
    app.setApplicationName("BMS Tool")

    if args.fake:
        # Start the fake target in a background thread
        from ..fake_target.fake_target import FakeTarget
        threading.Thread(
            target=FakeTarget.serve_tcp,
            args=('127.0.0.1', 65102, args.mode),
            daemon=True,
        ).start()

    win = BmsMainWindow()
    win.show()

    if args.fake:
        import time
        time.sleep(0.1)  # allow server to start
        win._on_connect_requested('127.0.0.1', 65102)

    return app.exec()


if __name__ == '__main__':
    sys.exit(main())
