"""dashboard.py — Pack-level summary: voltage, current, state, faults, polling controls."""
from PyQt6.QtWidgets import (
    QWidget, QVBoxLayout, QHBoxLayout, QGridLayout,
    QGroupBox, QLabel, QPushButton,
)
from PyQt6.QtCore import Qt, pyqtSignal

from ...core.app_state import AppState

_BMS_STATES = {0: "INIT", 1: "STANDBY", 2: "PRECHARGE",
               3: "DISCHARGE", 4: "CHARGE", 5: "FAULT"}


def _card(title: str, row: int, grid: QGridLayout) -> QLabel:
    """Add a label+value pair to a grid; return the value label."""
    lbl = QLabel(title)
    lbl.setStyleSheet("color:#555; font-size:11px;")
    val = QLabel("—")
    val.setStyleSheet("font-size:16px; font-weight:bold;")
    val.setAlignment(Qt.AlignmentFlag.AlignLeft | Qt.AlignmentFlag.AlignVCenter)
    grid.addWidget(lbl, row, 0, alignment=Qt.AlignmentFlag.AlignRight)
    grid.addWidget(val, row, 1)
    return val


class DashboardPage(QWidget):
    polling_toggle_requested = pyqtSignal()
    refresh_now_requested    = pyqtSignal()

    def __init__(self, state: AppState, parent=None):
        super().__init__(parent)
        self._polling_active = False
        self._build_ui()

    def _build_ui(self) -> None:
        layout = QVBoxLayout(self)
        layout.setContentsMargins(10, 10, 10, 10)
        layout.setSpacing(8)

        # ── Polling controls ──────────────────────────────────────────────────
        poll_grp = QGroupBox("Polling")
        poll_lay = QHBoxLayout(poll_grp)
        poll_lay.setContentsMargins(8, 4, 8, 4)

        self._poll_btn    = QPushButton("Stop Polling")
        self._refresh_btn = QPushButton("Refresh Now")
        self._poll_status = QLabel("Polling: active")
        self._poll_status.setStyleSheet("color:#2a6b2a; font-weight:bold;")

        self._poll_btn.clicked.connect(self._on_polling_toggle)
        self._refresh_btn.clicked.connect(self.refresh_now_requested)

        poll_lay.addWidget(self._poll_btn)
        poll_lay.addWidget(self._refresh_btn)
        poll_lay.addSpacing(16)
        poll_lay.addWidget(self._poll_status)
        poll_lay.addStretch()
        layout.addWidget(poll_grp)

        # ── Pack summary — left/right columns ────────────────────────────────
        summary_grp = QGroupBox("Pack Summary")
        summary_lay = QHBoxLayout(summary_grp)

        # Left: voltages + current
        left_grp = QGroupBox("Electrical")
        left_grid = QGridLayout(left_grp)
        left_grid.setColumnMinimumWidth(0, 110)
        self._vbat  = _card("Vbat:",        0, left_grid)
        self._vpack = _card("Vpack:",       1, left_grid)
        self._ibat  = _card("Current:",     2, left_grid)
        self._state = _card("BMS State:",   3, left_grid)
        self._uptime = _card("Uptime:",     4, left_grid)
        self._mflags = _card("Meas Flags:", 5, left_grid)

        # Right: faults + outputs
        right_grp = QGroupBox("Status")
        right_grid = QGridLayout(right_grp)
        right_grid.setColumnMinimumWidth(0, 110)
        self._fault_sum = _card("Active Faults:",  0, right_grid)
        self._latched   = _card("Latched Faults:", 1, right_grid)
        self._outputs   = _card("Outputs:",        2, right_grid)

        summary_lay.addWidget(left_grp)
        summary_lay.addWidget(right_grp)
        layout.addWidget(summary_grp)
        layout.addStretch()

    def _on_polling_toggle(self) -> None:
        self.polling_toggle_requested.emit()
        self._polling_active = not self._polling_active
        self._sync_poll_ui()

    def set_polling_active(self, active: bool) -> None:
        self._polling_active = active
        self._sync_poll_ui()

    def _sync_poll_ui(self) -> None:
        if self._polling_active:
            self._poll_btn.setText("Stop Polling")
            self._poll_status.setText("Polling: active")
            self._poll_status.setStyleSheet("color:#2a6b2a; font-weight:bold;")
        else:
            self._poll_btn.setText("Start Polling")
            self._poll_status.setText("Polling: stopped")
            self._poll_status.setStyleSheet("color:#9a6000; font-weight:bold;")

    def _dash(self, label: QLabel, text: str) -> None:
        label.setText(text)

    def refresh(self, state: AppState) -> None:
        vs = state.values
        if not vs.valid:
            for lbl in (self._vbat, self._vpack, self._ibat,
                        self._state, self._uptime, self._outputs,
                        self._fault_sum, self._latched, self._mflags):
                self._dash(lbl, "—")
            return

        self._vbat.setText(f"{vs.vbat_mv} mV")
        self._vpack.setText(f"{vs.vpack_mv} mV")
        self._ibat.setText(f"{vs.i_batt_ma} mA")
        self._state.setText(_BMS_STATES.get(vs.bms_state, str(vs.bms_state)))
        self._uptime.setText(f"{vs.uptime_ms / 1000:.1f} s")
        self._outputs.setText(f"0x{vs.outputs_state:02X}")
        self._mflags.setText(f"0x{vs.measurement_flags:02X}")

        n_active = bin(vs.active_faults).count('1')
        fa_text  = f"{n_active} active" if n_active else "none"
        self._fault_sum.setText(fa_text)
        self._fault_sum.setStyleSheet(
            "font-size:16px; font-weight:bold; color:#8a0000;"
            if n_active else
            "font-size:16px; font-weight:bold; color:#2a6b2a;")

        n_latched = bin(vs.latched_faults).count('1')
        lat_text  = f"{n_latched} latched" if n_latched else "none"
        self._latched.setText(lat_text)
        self._latched.setStyleSheet(
            "font-size:16px; font-weight:bold; color:#9a6000;"
            if n_latched else
            "font-size:16px; font-weight:bold; color:#2a6b2a;")
