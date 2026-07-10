"""main.py — BMS desktop tool GUI entry point."""
import sys
import threading
from typing import Optional

from PyQt6.QtWidgets import (
    QApplication, QMainWindow, QTabWidget, QWidget,
    QLabel, QVBoxLayout, QHBoxLayout,
)
from PyQt6.QtCore import Qt, pyqtSignal, QObject

from ..core.app_state import AppState
from ..core.connection_manager import ConnectionManager
from ..core.target_model import TargetModel
from ..core.polling import PollingLoop
from ..core.logging_model import EventLog, PacketLog
from ..connection.device_state import DeviceState, DeviceMode

from .pages.connection import ConnectionPage
from .pages.bringup import BringupPage
from .pages.dashboard import DashboardPage
from .pages.cells import CellsPage
from .pages.temperatures import TemperaturesPage
from .pages.faults import FaultsPage
from .pages.charging import ChargingPage
from .pages.config import ConfigPage
from .pages.firmware_flash import FirmwareFlashPage
from .pages.logs import LogsPage
from .style import mode_badge_style, APP_STYLESHEET


class _StateSignals(QObject):
    """Cross-thread state-change delivery: polling thread → main thread."""
    updated = pyqtSignal(str)


class BmsMainWindow(QMainWindow):
    def __init__(self):
        super().__init__()
        self.setWindowTitle("BMS Tool")
        self.resize(1280, 820)

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
        root = QWidget()
        root_lay = QVBoxLayout(root)
        root_lay.setContentsMargins(0, 0, 0, 0)
        root_lay.setSpacing(0)

        # ── Top status banner ─────────────────────────────────────────────────
        self._banner = QWidget()
        self._banner.setFixedHeight(34)
        self._banner.setAutoFillBackground(True)
        banner_lay = QHBoxLayout(self._banner)
        banner_lay.setContentsMargins(10, 0, 10, 0)

        self._mode_badge = QLabel("DISCONNECTED")
        self._mode_badge.setStyleSheet(mode_badge_style('DISCONNECTED'))
        banner_lay.addWidget(self._mode_badge)
        banner_lay.addSpacing(12)

        self._banner_info = QLabel("")
        banner_lay.addWidget(self._banner_info)
        banner_lay.addStretch()

        self._banner_warn = QLabel("")
        self._banner_warn.setStyleSheet(
            "color:#ffffff; font-weight:bold; background:#9a6000; "
            "padding:2px 10px; border-radius:3px;")
        self._banner_warn.setVisible(False)
        banner_lay.addWidget(self._banner_warn)

        self._banner.setStyleSheet("background:#333333;")
        self._banner_info.setStyleSheet("color:#cccccc; font-size:12px;")
        root_lay.addWidget(self._banner)

        # ── Tab widget ────────────────────────────────────────────────────────
        tabs = QTabWidget()
        self._tabs = tabs

        self._page_conn    = ConnectionPage(self._state, self)
        self._page_bringup = BringupPage(self._state, self)
        self._page_dash    = DashboardPage(self._state)
        self._page_cells   = CellsPage(self._state)
        self._page_temps   = TemperaturesPage(self._state)
        self._page_fault   = FaultsPage(self._state, self)
        self._page_charge  = ChargingPage(self._state)
        self._page_cfg     = ConfigPage(self._state, self)
        self._page_flash   = FirmwareFlashPage(self._state, self)
        self._page_logs    = LogsPage(self._evt_log, self._pkt_log)

        tabs.addTab(self._page_conn,    "Connection")
        tabs.addTab(self._page_bringup, "Bring-Up")
        tabs.addTab(self._page_dash,    "Dashboard")
        tabs.addTab(self._page_cells,   "Cells")
        tabs.addTab(self._page_temps,   "Temperatures")
        tabs.addTab(self._page_fault,   "Faults")
        tabs.addTab(self._page_charge,  "Charging")
        tabs.addTab(self._page_cfg,     "Config")
        tabs.addTab(self._page_flash,   "Firmware Flash")
        tabs.addTab(self._page_logs,    "Logs")

        root_lay.addWidget(tabs, 1)
        self.setCentralWidget(root)

        # ── Signal wiring ─────────────────────────────────────────────────────
        self._page_conn.connect_requested.connect(self._on_connect_tcp_requested)
        self._page_conn.connect_serial_requested.connect(self._on_connect_serial_requested)
        self._page_conn.disconnect_requested.connect(self._on_disconnect)

        self._page_fault.clear_latched_requested.connect(self._on_clear_latched)
        self._page_fault.refresh_requested.connect(self._on_refresh_faults)

        self._page_dash.polling_toggle_requested.connect(self._on_polling_toggle)
        self._page_dash.refresh_now_requested.connect(self._on_refresh_now)

        self._page_cells.measure_once_requested.connect(self._on_measure_cells_once)
        self._page_cells.refresh_requested.connect(self._on_refresh_cells)

        self._page_temps.measure_once_requested.connect(self._on_measure_temps_once)
        self._page_temps.refresh_requested.connect(self._on_refresh_temps)

        self._page_charge.refresh_requested.connect(self._on_refresh_charging)

    # ── Banner update ─────────────────────────────────────────────────────────

    def _update_banner(self, device: DeviceState) -> None:
        mode_name = device.mode.name
        self._mode_badge.setText(mode_name)
        self._mode_badge.setStyleSheet(mode_badge_style(mode_name))

        caps = device.capabilities
        if caps:
            fw_ver = '.'.join(str(x) for x in caps.firmware_version)
            info = (
                f"FW v{fw_ver}  |  "
                f"HW 0x{caps.hw_profile_id:04X}  |  "
                f"{caps.cell_count} cells / {caps.temp_count} temps"
            )
            if device.error_msg:
                info += f"  |  {device.error_msg}"
            self._banner_info.setText(info)
        else:
            self._banner_info.setText(device.error_msg or "")

        is_bl = (device.mode == DeviceMode.BOOTLOADER)
        self._banner_warn.setVisible(is_bl)
        if is_bl:
            self._banner_warn.setText(
                "BOOTLOADER MODE — runtime telemetry disabled, firmware update only")

    # ── Connect helpers ───────────────────────────────────────────────────────

    def _finish_connect(self, transport) -> None:
        self._model  = TargetModel(transport)
        device       = self._model.capabilities_handshake()
        self._state.update_device(device)
        self._evt_log.append(f"Connected: mode={device.mode.name}")

        if device.mode == DeviceMode.BMS_APP:
            # Config doesn't live-poll like the other tabs — without this,
            # the Config page stays blank/default even though a valid
            # config is stored on the target until the user clicks "Read".
            # Must run BEFORE PollingLoop starts its background thread: the
            # protocol has one request in flight at a time over one serial
            # port, and reading concurrently from two threads causes one
            # side's response bytes to be stolen by the other reader.
            self._page_cfg.read_from_target()
            self._polling = PollingLoop(self._model, self._state, self._evt_log)
            self._polling.start()
            self._page_dash.set_polling_active(True)

        # Always resync the connection page so button enable-state matches
        # the actual device mode (handshake may have failed → UNKNOWN).
        self._page_conn.refresh(self._state)

    def _on_connect_tcp_requested(self, host: str, port: int) -> None:
        self._evt_log.append(f"Connecting TCP → {host}:{port} …")
        try:
            transport = self._conn_mgr.connect_tcp(host, port)
        except (OSError, IOError) as e:
            self._evt_log.append(f"TCP connection failed: {e}")
            self._page_conn.refresh(self._state)
            return
        self._finish_connect(transport)

    def _on_connect_serial_requested(self, device: str, baud: int) -> None:
        self._evt_log.append(f"Connecting serial → {device} @ {baud} …")
        try:
            transport = self._conn_mgr.connect_serial(device, baud)
        except Exception as e:
            self._evt_log.append(f"Serial connection failed: {e}")
            self._page_conn.refresh(self._state)
            return
        self._finish_connect(transport)

    def _on_disconnect(self) -> None:
        if self._polling:
            self._polling.stop()
            self._polling = None
        self._conn_mgr.disconnect()
        self._model = None
        self._state.reset()
        self._page_dash.set_polling_active(False)
        self._evt_log.append("Disconnected.")
        # Resync page buttons (re-enable Connect, disable Disconnect)
        self._page_conn.refresh(self._state)

    # ── Fault actions ─────────────────────────────────────────────────────────

    def _on_clear_latched(self, mask: int) -> None:
        if self._model is None:
            return
        try:
            cleared = self._model.clear_latched_faults(mask)
            self._evt_log.append(f"Cleared latched faults: 0x{cleared:016X}")
        except Exception as e:
            self._evt_log.append(f"clear_latched_faults error: {e}")

    def _on_refresh_faults(self) -> None:
        if self._model is None:
            return
        try:
            fs = self._model.poll_faults()
            self._state.update_faults(fs)
        except Exception as e:
            self._evt_log.append(f"refresh_faults error: {e}")

    # ── Polling controls ──────────────────────────────────────────────────────

    def _on_polling_toggle(self) -> None:
        if self._polling is None:
            if self._model and self._state.device.mode == DeviceMode.BMS_APP:
                self._polling = PollingLoop(self._model, self._state, self._evt_log)
                self._polling.start()
        else:
            self._polling.stop()
            self._polling = None

    def _on_refresh_now(self) -> None:
        if self._model is None:
            return
        try:
            self._state.update_values(self._model.poll_values())
        except Exception as e:
            self._evt_log.append(f"refresh_now error: {e}")

    # ── One-shot measurements ─────────────────────────────────────────────────

    def _on_measure_cells_once(self) -> None:
        if self._model is None:
            return
        try:
            r  = self._model.measure_cells_once()
            from ..core.app_state import CellsState
            cs = CellsState(
                cell_count   = r['cell_count'],
                cells_mv     = r['cells_mv'],
                validity     = r.get('validity'),
                timestamp_ms = r['timestamp_ms'],
                valid        = True,
            )
            self._state.update_cells(cs)
        except Exception as e:
            self._evt_log.append(f"measure_cells_once error: {e}")

    def _on_measure_temps_once(self) -> None:
        if self._model is None:
            return
        try:
            r  = self._model.measure_temps_once()
            from ..core.app_state import TempsState
            ts = TempsState(
                temp_count = r['temp_count'],
                temps_cx10 = r['temps_cx10'],
                valid      = True,
            )
            self._state.update_temps(ts)
        except Exception as e:
            self._evt_log.append(f"measure_temps_once error: {e}")

    def _on_refresh_cells(self) -> None:
        if self._model is None:
            return
        try:
            self._state.update_cells(self._model.poll_cells())
        except Exception as e:
            self._evt_log.append(f"refresh_cells error: {e}")

    def _on_refresh_temps(self) -> None:
        if self._model is None:
            return
        try:
            self._state.update_temps(self._model.poll_temps())
        except Exception as e:
            self._evt_log.append(f"refresh_temps error: {e}")

    def _on_refresh_charging(self) -> None:
        if self._model is None:
            return
        try:
            self._state.update_values(self._model.poll_values())
            self._state.update_charger(self._model.poll_charger_status())
        except Exception as e:
            self._evt_log.append(f"refresh_charging error: {e}")

    # ── State update dispatch ─────────────────────────────────────────────────

    def _on_state_updated(self, key: str) -> None:
        """Called on the main thread whenever AppState changes."""
        device = self._state.device

        if key == 'device':
            # Stop polling immediately when leaving BMS_APP mode.
            # This prevents TargetRefusedError spam in the event log when the
            # device enters BOOTLOADER mode or disconnects.
            if device.mode != DeviceMode.BMS_APP and self._polling is not None:
                self._polling.stop()
                self._polling = None
                self._page_dash.set_polling_active(False)

            self._update_banner(device)
            self._page_conn.refresh(self._state)

        self._page_bringup.refresh(self._state)
        self._page_dash.refresh(self._state)
        self._page_cells.refresh(self._state)
        self._page_temps.refresh(self._state)
        self._page_fault.refresh(self._state)
        self._page_charge.refresh(self._state)
        self._page_cfg.refresh(self._state)
        self._page_flash.refresh(self._state)
        self._page_logs.refresh()

        # Tab indices: 0=Connection, 1=Bring-Up, 2=Dashboard, 3=Cells,
        #              4=Temps, 5=Faults, 6=Charging, 7=Config,
        #              8=Firmware Flash, 9=Logs
        is_app = (device.mode == DeviceMode.BMS_APP)
        is_bl  = (device.mode == DeviceMode.BOOTLOADER)
        is_any = is_app or is_bl

        tab_enabled = [
            True,    # Connection: always
            is_any,  # Bring-Up: app or bootloader
            is_app,  # Dashboard
            is_app,  # Cells
            is_app,  # Temperatures
            is_app,  # Faults
            is_app,  # Charging
            is_app,  # Config
            is_any,  # Firmware Flash: app or bootloader
            True,    # Logs: always
        ]
        for idx, enabled in enumerate(tab_enabled):
            self._tabs.setTabEnabled(idx, enabled)

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
    app.setStyleSheet(APP_STYLESHEET)

    if args.fake:
        from ..fake_target.live_simulator import LIVE_MODES, LiveFakeHardware
        from ..fake_target.fake_target import FakeTarget
        if args.mode in LIVE_MODES:
            threading.Thread(
                target=LiveFakeHardware.serve_tcp,
                args=('127.0.0.1', 65102),
                kwargs={'mode': args.mode},
                daemon=True,
            ).start()
        else:
            threading.Thread(
                target=FakeTarget.serve_tcp,
                args=('127.0.0.1', 65102, args.mode),
                daemon=True,
            ).start()

    win = BmsMainWindow()
    win.show()

    if args.fake:
        import time
        time.sleep(0.1)
        win._on_connect_tcp_requested('127.0.0.1', 65102)

    return app.exec()


if __name__ == '__main__':
    sys.exit(main())
