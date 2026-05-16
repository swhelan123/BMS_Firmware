"""faults.py — active/latched fault bitmaps with named fault list."""
from PyQt6.QtWidgets import (
    QWidget, QVBoxLayout, QHBoxLayout, QLabel, QPushButton,
    QTableWidget, QTableWidgetItem, QGroupBox,
)
from PyQt6.QtCore import pyqtSignal, Qt
from PyQt6.QtGui import QColor

from ...core.app_state import AppState

_ACTIVE_COLOR  = QColor(255, 80,  80)
_LATCHED_COLOR = QColor(255, 180, 80)
_CLEAR_COLOR   = QColor(220, 255, 220)

_FAULT_NAMES = [
    "CELL_OV", "CELL_UV", "CELL_OV_SOFT", "CELL_UV_SOFT",
    "CELL_READ_INVALID", "CELL_OPENWIRE", "TEMP_OVER_CHARGE", "TEMP_OVER_DISCHARGE",
    "TEMP_OVER_ABS", "TEMP_READ_INVALID", "TEMP_COVERAGE", "VBAT_INVALID",
    "VPACK_INVALID", "PRECHARGE_TIMEOUT", "PRECHARGE_DELTA", "ISOSPI_CELL",
    "ISOSPI_TEMP", "I2C_ISL28022", "WATCHDOG", "CONFIG_INVALID",
    "OVERCURRENT", "BALANCE_TEMP_VIOLATION", "TEMP_CHAIN_BALANCE_ATTEMPT",
    "TEMP_COLD_CHARGE", "TEMP_COLD_DISCHARGE",
]


class FaultsPage(QWidget):
    clear_latched_requested = pyqtSignal(int)  # mask of faults to clear
    refresh_requested       = pyqtSignal()

    def __init__(self, state: AppState, parent=None):
        super().__init__(parent)
        self._build_ui()

    def _build_ui(self) -> None:
        layout = QVBoxLayout(self)

        # Raw bitmaps
        raw_grp = QGroupBox("Raw Fault Bitmaps")
        raw_lay = QVBoxLayout(raw_grp)
        self._active_lbl  = QLabel("Active:  0x0000000000000000")
        self._latched_lbl = QLabel("Latched: 0x0000000000000000")
        raw_lay.addWidget(self._active_lbl)
        raw_lay.addWidget(self._latched_lbl)
        layout.addWidget(raw_grp)

        # Fault table
        self._table = QTableWidget(0, 3)
        self._table.setHorizontalHeaderLabels(["Bit", "Name", "State"])
        self._table.setEditTriggers(QTableWidget.EditTrigger.NoEditTriggers)
        layout.addWidget(self._table)

        # Buttons
        btn_lay = QHBoxLayout()
        self._refresh_btn = QPushButton("Refresh")
        self._clear_btn   = QPushButton("Clear All Latched Faults")
        self._clear_btn.setEnabled(False)
        self._refresh_btn.clicked.connect(self.refresh_requested)
        self._clear_btn.clicked.connect(
            lambda: self.clear_latched_requested.emit(0xFFFFFFFFFFFFFFFF))
        btn_lay.addWidget(self._refresh_btn)
        btn_lay.addWidget(self._clear_btn)
        btn_lay.addStretch()
        layout.addLayout(btn_lay)

    def refresh(self, state: AppState) -> None:
        fs = state.faults
        if not fs.valid:
            return

        active  = fs.active_faults
        latched = fs.latched_faults

        self._active_lbl.setText( f"Active:  0x{active:016X}")
        self._latched_lbl.setText(f"Latched: 0x{latched:016X}")
        self._clear_btn.setEnabled(bool(latched))

        interesting = [i for i in range(64) if (active | latched) & (1 << i)]
        self._table.setRowCount(len(interesting))
        for row, bit in enumerate(interesting):
            is_active  = bool(active  & (1 << bit))
            is_latched = bool(latched & (1 << bit))
            name       = _FAULT_NAMES[bit] if bit < len(_FAULT_NAMES) else f"BIT_{bit}"
            state_txt  = "ACTIVE+LATCHED" if (is_active and is_latched) else \
                         "ACTIVE"         if is_active  else "LATCHED"
            color = _ACTIVE_COLOR if is_active else _LATCHED_COLOR

            for col, text in enumerate([str(bit), name, state_txt]):
                item = QTableWidgetItem(text)
                item.setBackground(color)
                item.setTextAlignment(Qt.AlignmentFlag.AlignCenter)
                self._table.setItem(row, col, item)

        self._table.resizeColumnsToContents()
