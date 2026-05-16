"""dashboard.py — Pack-level summary: voltage, current, SOC, state, faults."""
from PyQt6.QtWidgets import (
    QWidget, QVBoxLayout, QGridLayout, QGroupBox, QLabel,
)
from PyQt6.QtCore import Qt

from ...core.app_state import AppState

_BMS_STATES = {0: "INIT", 1: "STANDBY", 2: "PRECHARGE",
               3: "DISCHARGE", 4: "CHARGE", 5: "FAULT"}


class DashboardPage(QWidget):
    def __init__(self, state: AppState, parent=None):
        super().__init__(parent)
        self._build_ui()

    def _build_ui(self) -> None:
        layout = QVBoxLayout(self)

        grp  = QGroupBox("Pack Summary")
        grid = QGridLayout(grp)

        def row(label: str, row: int) -> QLabel:
            grid.addWidget(QLabel(label), row, 0, alignment=Qt.AlignmentFlag.AlignRight)
            val = QLabel("—")
            grid.addWidget(val, row, 1)
            return val

        self._vbat    = row("Vbat (mV):",    0)
        self._vpack   = row("Vpack (mV):",   1)
        self._ibat    = row("Current (mA):", 2)
        self._state   = row("BMS State:",    3)
        self._uptime  = row("Uptime (s):",   4)
        self._outputs = row("Outputs:",      5)
        self._fault_sum = row("Active Faults:", 6)
        self._mflags  = row("Meas Flags:",   7)

        layout.addWidget(grp)
        layout.addStretch()

    def refresh(self, state: AppState) -> None:
        vs = state.values
        if not vs.valid:
            return
        self._vbat.setText(str(vs.vbat_mv))
        self._vpack.setText(str(vs.vpack_mv))
        self._ibat.setText(str(vs.i_batt_ma))
        self._state.setText(_BMS_STATES.get(vs.bms_state, str(vs.bms_state)))
        self._uptime.setText(f"{vs.uptime_ms / 1000:.1f}")
        self._outputs.setText(f"0x{vs.outputs_state:02X}")
        n_faults = bin(vs.active_faults).count('1')
        self._fault_sum.setText(
            f"{n_faults} active" if n_faults else "none")
        self._mflags.setText(f"0x{vs.measurement_flags:02X}")
