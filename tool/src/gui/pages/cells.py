"""cells.py — 75-cell voltage table with min/max/avg/mismatch summary."""
from PyQt6.QtWidgets import (
    QWidget, QVBoxLayout, QHBoxLayout, QLabel,
    QTableWidget, QTableWidgetItem, QGroupBox,
)
from PyQt6.QtCore import Qt
from PyQt6.QtGui import QColor

from ...core.app_state import AppState

_INVALID_COLOR = QColor(255, 80, 80)
_NORMAL_COLOR  = QColor(255, 255, 255)


class CellsPage(QWidget):
    def __init__(self, state: AppState, parent=None):
        super().__init__(parent)
        self._build_ui()

    def _build_ui(self) -> None:
        layout = QVBoxLayout(self)

        # Summary row
        sum_grp = QGroupBox("Summary")
        sum_lay = QHBoxLayout(sum_grp)
        self._min_lbl  = QLabel("Min: —")
        self._max_lbl  = QLabel("Max: —")
        self._avg_lbl  = QLabel("Avg: —")
        self._mm_lbl   = QLabel("Mismatch: —")
        self._warn_lbl = QLabel("")
        self._warn_lbl.setStyleSheet("color: orange; font-weight: bold;")
        for w in (self._min_lbl, self._max_lbl, self._avg_lbl,
                  self._mm_lbl, self._warn_lbl):
            sum_lay.addWidget(w)
        sum_lay.addStretch()
        layout.addWidget(sum_grp)

        # Cell table: 15 rows × 5 cols
        self._table = QTableWidget(15, 5)
        self._table.setHorizontalHeaderLabels(
            [f"Cells {i*15}–{i*15+14}" for i in range(5)])
        self._table.verticalHeader().setVisible(False)
        self._table.setEditTriggers(QTableWidget.EditTrigger.NoEditTriggers)
        layout.addWidget(self._table)

    def refresh(self, state: AppState) -> None:
        cs = state.cells
        if not cs.valid or not cs.cells_mv:
            return

        mv = cs.cells_mv
        valid_mv = [v for i, v in enumerate(mv)
                    if cs.validity is None or (i < len(cs.validity) and cs.validity[i])]

        if valid_mv:
            self._min_lbl.setText(f"Min: {min(valid_mv)} mV")
            self._max_lbl.setText(f"Max: {max(valid_mv)} mV")
            self._avg_lbl.setText(f"Avg: {sum(valid_mv)//len(valid_mv)} mV")
            self._mm_lbl.setText( f"Mismatch: {max(valid_mv)-min(valid_mv)} mV")
        invalid_count = cs.cell_count - len(valid_mv)
        self._warn_lbl.setText(
            f"⚠ {invalid_count} invalid cells" if invalid_count else "")

        for idx, v in enumerate(mv[:75]):
            row = idx % 15
            col = idx // 15
            is_valid = (cs.validity is None or
                        (idx < len(cs.validity) and cs.validity[idx]))
            item = QTableWidgetItem(f"[{idx:02d}] {v}")
            item.setTextAlignment(Qt.AlignmentFlag.AlignCenter)
            if not is_valid:
                item.setBackground(_INVALID_COLOR)
            self._table.setItem(row, col, item)

        self._table.resizeColumnsToContents()
