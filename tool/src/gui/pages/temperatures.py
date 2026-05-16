"""temperatures.py — 75-temperature table with high/avg/low summary."""
from PyQt6.QtWidgets import (
    QWidget, QVBoxLayout, QHBoxLayout, QLabel,
    QTableWidget, QTableWidgetItem, QGroupBox,
)
from PyQt6.QtCore import Qt
from PyQt6.QtGui import QColor

from ...core.app_state import AppState

TEMP_INVALID = -0x8000
_INVALID_COLOR = QColor(255, 80, 80)


class TemperaturesPage(QWidget):
    def __init__(self, state: AppState, parent=None):
        super().__init__(parent)
        self._build_ui()

    def _build_ui(self) -> None:
        layout = QVBoxLayout(self)

        sum_grp = QGroupBox("Summary")
        sum_lay = QHBoxLayout(sum_grp)
        self._max_lbl  = QLabel("Max: —")
        self._avg_lbl  = QLabel("Avg: —")
        self._min_lbl  = QLabel("Min: —")
        self._warn_lbl = QLabel("")
        self._warn_lbl.setStyleSheet("color: orange; font-weight: bold;")
        for w in (self._max_lbl, self._avg_lbl, self._min_lbl, self._warn_lbl):
            sum_lay.addWidget(w)
        sum_lay.addStretch()
        layout.addWidget(sum_grp)

        self._table = QTableWidget(15, 5)
        self._table.setHorizontalHeaderLabels(
            [f"Temps {i*15}–{i*15+14}" for i in range(5)])
        self._table.verticalHeader().setVisible(False)
        self._table.setEditTriggers(QTableWidget.EditTrigger.NoEditTriggers)
        layout.addWidget(self._table)

    def refresh(self, state: AppState) -> None:
        ts = state.temps
        if not ts.valid or not ts.temps_cx10:
            return

        valid = [t for t in ts.temps_cx10 if t != TEMP_INVALID]
        if valid:
            self._max_lbl.setText(f"Max: {max(valid)/10:.1f}°C")
            self._avg_lbl.setText(f"Avg: {sum(valid)/len(valid)/10:.1f}°C")
            self._min_lbl.setText(f"Min: {min(valid)/10:.1f}°C")

        inv = ts.temp_count - len(valid)
        self._warn_lbl.setText(f"⚠ {inv} invalid sensors" if inv else "")

        for idx, t in enumerate(ts.temps_cx10[:75]):
            row = idx % 15
            col = idx // 15
            if t == TEMP_INVALID:
                text = f"[{idx:02d}] INVALID"
                item = QTableWidgetItem(text)
                item.setBackground(_INVALID_COLOR)
            else:
                item = QTableWidgetItem(f"[{idx:02d}] {t/10:.1f}°C")
            item.setTextAlignment(Qt.AlignmentFlag.AlignCenter)
            self._table.setItem(row, col, item)

        self._table.resizeColumnsToContents()
