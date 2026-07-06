"""temperatures.py — 75-temperature table with high/avg/low summary."""
from PyQt6.QtWidgets import (
    QWidget, QVBoxLayout, QHBoxLayout, QLabel,
    QTableWidget, QTableWidgetItem, QGroupBox, QPushButton, QCheckBox,
)
from PyQt6.QtCore import Qt, pyqtSignal
from PyQt6.QtGui import QColor

from ...core.app_state import AppState

TEMP_INVALID   = -0x8000
_INVALID_COLOR = QColor(180, 40, 40)
_INVALID_TEXT  = QColor(255, 255, 255)


class TemperaturesPage(QWidget):
    measure_once_requested = pyqtSignal()
    refresh_requested      = pyqtSignal()

    def __init__(self, state: AppState, parent=None):
        super().__init__(parent)
        self._build_ui()

    def _build_ui(self) -> None:
        layout = QVBoxLayout(self)

        # Action buttons
        act_grp = QGroupBox("Actions")
        act_lay = QHBoxLayout(act_grp)
        self._meas_btn    = QPushButton("Measure Temps Once")
        self._refresh_btn = QPushButton("Refresh Snapshot")
        self._raw_chk     = QCheckBox("Show raw mV")
        self._raw_chk.setToolTip(
            "Show the raw C-input voltage (mV) each channel measured, "
            "alongside the converted temperature — diagnostic for temp "
            "sensor acquisition.")
        self._meas_btn.clicked.connect(   self.measure_once_requested)
        self._refresh_btn.clicked.connect(self.refresh_requested)
        self._raw_chk.toggled.connect(self._on_raw_toggled)
        act_lay.addWidget(self._meas_btn)
        act_lay.addWidget(self._refresh_btn)
        act_lay.addWidget(self._raw_chk)
        act_lay.addStretch()
        layout.addWidget(act_grp)
        self._last_state = None

        # Summary row
        sum_grp = QGroupBox("Summary")
        sum_lay = QHBoxLayout(sum_grp)
        self._max_lbl  = QLabel("Max: —")
        self._avg_lbl  = QLabel("Avg: —")
        self._min_lbl  = QLabel("Min: —")
        self._ts_lbl   = QLabel("Snapshot: —")
        self._warn_lbl = QLabel("")
        self._warn_lbl.setStyleSheet("color: #d4a017; font-weight: bold;")
        for w in (self._max_lbl, self._avg_lbl, self._min_lbl,
                  self._ts_lbl, self._warn_lbl):
            sum_lay.addWidget(w)
        sum_lay.addStretch()
        layout.addWidget(sum_grp)

        # One column per populated segment; follows target temp_count.
        self._table = QTableWidget(15, 5)
        self._segments = 0
        self._set_segments(5)
        self._table.verticalHeader().setVisible(False)
        self._table.setEditTriggers(QTableWidget.EditTrigger.NoEditTriggers)
        layout.addWidget(self._table)

    def _set_segments(self, n: int) -> None:
        n = max(1, min(5, n))
        if n == self._segments:
            return
        self._segments = n
        self._table.setColumnCount(n)
        self._table.setHorizontalHeaderLabels(
            [f"Temps {i*15}–{i*15+14}" for i in range(n)])

    def _on_raw_toggled(self, _checked: bool) -> None:
        if self._last_state is not None:
            self.refresh(self._last_state)

    def refresh(self, state: AppState) -> None:
        self._last_state = state
        ts = state.temps
        if not ts.valid or not ts.temps_cx10:
            return

        valid = [t for t in ts.temps_cx10 if t != TEMP_INVALID]
        if valid:
            self._max_lbl.setText(f"Max: {max(valid)/10:.1f}°C")
            self._avg_lbl.setText(f"Avg: {sum(valid)/len(valid)/10:.1f}°C")
            self._min_lbl.setText(f"Min: {min(valid)/10:.1f}°C")

        # TempsState doesn't carry a timestamp field; show placeholder
        self._ts_lbl.setText("Snapshot: —")

        inv = ts.temp_count - len(valid)
        self._warn_lbl.setText(f"⚠ {inv} invalid sensors" if inv else "")

        self._set_segments((ts.temp_count + 14) // 15 if ts.temp_count else 5)

        show_raw = self._raw_chk.isChecked()
        for idx, t in enumerate(ts.temps_cx10[:ts.temp_count]):
            row = idx % 15
            col = idx // 15
            raw_suffix = ""
            if show_raw and idx < len(ts.raw_mv):
                raw_suffix = f"  ({ts.raw_mv[idx]}mV)"
            if t == TEMP_INVALID:
                item = QTableWidgetItem(f"[{idx:02d}] INVALID{raw_suffix}")
                item.setBackground(_INVALID_COLOR)
                item.setForeground(_INVALID_TEXT)
            else:
                item = QTableWidgetItem(f"[{idx:02d}] {t/10:.1f}°C{raw_suffix}")
            item.setTextAlignment(Qt.AlignmentFlag.AlignCenter)
            self._table.setItem(row, col, item)

        self._table.resizeColumnsToContents()
