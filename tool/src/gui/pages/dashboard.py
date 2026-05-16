"""dashboard.py — Pack-level summary: voltage, current, state, faults, polling controls."""
from PyQt6.QtWidgets import (
    QWidget, QVBoxLayout, QHBoxLayout, QGridLayout, QGroupBox, QLabel, QPushButton,
)
from PyQt6.QtCore import Qt, pyqtSignal

from ...core.app_state import AppState

_BMS_STATES = {0: "INIT", 1: "STANDBY", 2: "PRECHARGE",
               3: "DISCHARGE", 4: "CHARGE", 5: "FAULT"}


class DashboardPage(QWidget):
    polling_toggle_requested = pyqtSignal()
    refresh_now_requested    = pyqtSignal()

    def __init__(self, state: AppState, parent=None):
        super().__init__(parent)
        self._polling_active = False
        self._build_ui()

    def _build_ui(self) -> None:
        layout = QVBoxLayout(self)

        # ── Polling controls ──────────────────────────────────────────────────
        poll_grp = QGroupBox("Polling")
        poll_lay = QHBoxLayout(poll_grp)

        self._poll_btn    = QPushButton("Stop Polling")
        self._refresh_btn = QPushButton("Refresh Now")
        self._poll_status = QLabel("Polling: active")

        self._poll_btn.clicked.connect(self._on_polling_toggle)
        self._refresh_btn.clicked.connect(self.refresh_now_requested)

        poll_lay.addWidget(self._poll_btn)
        poll_lay.addWidget(self._refresh_btn)
        poll_lay.addSpacing(12)
        poll_lay.addWidget(self._poll_status)
        poll_lay.addStretch()
        layout.addWidget(poll_grp)

        # ── Pack summary ──────────────────────────────────────────────────────
        grp  = QGroupBox("Pack Summary")
        grid = QGridLayout(grp)

        def row(label: str, r: int) -> QLabel:
            grid.addWidget(QLabel(label), r, 0,
                           alignment=Qt.AlignmentFlag.AlignRight)
            val = QLabel("—")
            grid.addWidget(val, r, 1)
            return val

        self._vbat       = row("Vbat (mV):",        0)
        self._vpack      = row("Vpack (mV):",        1)
        self._ibat       = row("Current (mA):",      2)
        self._state      = row("BMS State:",         3)
        self._uptime     = row("Uptime (s):",        4)
        self._outputs    = row("Outputs:",           5)
        self._fault_sum  = row("Active Faults:",     6)
        self._latched    = row("Latched Faults:",    7)
        self._mflags     = row("Meas Flags:",        8)

        layout.addWidget(grp)
        layout.addStretch()

    def _on_polling_toggle(self) -> None:
        self.polling_toggle_requested.emit()
        self._polling_active = not self._polling_active
        if self._polling_active:
            self._poll_btn.setText("Stop Polling")
            self._poll_status.setText("Polling: active")
        else:
            self._poll_btn.setText("Start Polling")
            self._poll_status.setText("Polling: stopped")

    def set_polling_active(self, active: bool) -> None:
        """Called by main window to sync polling state (e.g. on connect/disconnect)."""
        self._polling_active = active
        if active:
            self._poll_btn.setText("Stop Polling")
            self._poll_status.setText("Polling: active")
        else:
            self._poll_btn.setText("Start Polling")
            self._poll_status.setText("Polling: stopped")

    def refresh(self, state: AppState) -> None:
        vs = state.values
        if not vs.valid:
            for lbl in (self._vbat, self._vpack, self._ibat,
                        self._state, self._uptime, self._outputs,
                        self._fault_sum, self._latched, self._mflags):
                lbl.setText("—")
            return

        self._vbat.setText(str(vs.vbat_mv))
        self._vpack.setText(str(vs.vpack_mv))
        self._ibat.setText(str(vs.i_batt_ma))
        self._state.setText(_BMS_STATES.get(vs.bms_state, str(vs.bms_state)))
        self._uptime.setText(f"{vs.uptime_ms / 1000:.1f}")
        self._outputs.setText(f"0x{vs.outputs_state:02X}")

        n_active = bin(vs.active_faults).count('1')
        self._fault_sum.setText(
            f"{n_active} active" if n_active else "none")

        n_latched = bin(vs.latched_faults).count('1')
        self._latched.setText(
            f"{n_latched} latched" if n_latched else "none")

        self._mflags.setText(f"0x{vs.measurement_flags:02X}")
