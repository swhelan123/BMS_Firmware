"""cells.py — 75-cell voltage table with min/max/avg/mismatch summary."""
from PyQt6.QtWidgets import (
    QWidget, QVBoxLayout, QHBoxLayout, QLabel,
    QTableWidget, QTableWidgetItem, QGroupBox, QPushButton,
)
from PyQt6.QtCore import Qt, pyqtSignal
from PyQt6.QtGui import QColor

from ...core.app_state import AppState

_INVALID_COLOR = QColor(180, 40, 40)
_INVALID_TEXT  = QColor(255, 255, 255)


class CellsPage(QWidget):
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
        self._meas_btn    = QPushButton("Measure Cells Once")
        self._refresh_btn = QPushButton("Refresh Snapshot")
        self._meas_btn.clicked.connect(   self.measure_once_requested)
        self._refresh_btn.clicked.connect(self.refresh_requested)
        act_lay.addWidget(self._meas_btn)
        act_lay.addWidget(self._refresh_btn)
        act_lay.addStretch()
        layout.addWidget(act_grp)

        # Summary row
        sum_grp = QGroupBox("Summary")
        sum_lay = QHBoxLayout(sum_grp)
        self._min_lbl  = QLabel("Min: —")
        self._max_lbl  = QLabel("Max: —")
        self._avg_lbl  = QLabel("Avg: —")
        self._mm_lbl   = QLabel("Mismatch: —")
        self._ts_lbl   = QLabel("Snapshot: —")
        self._warn_lbl = QLabel("")
        self._warn_lbl.setStyleSheet("color: #d4a017; font-weight: bold;")
        for w in (self._min_lbl, self._max_lbl, self._avg_lbl,
                  self._mm_lbl, self._ts_lbl, self._warn_lbl):
            sum_lay.addWidget(w)
        sum_lay.addStretch()
        layout.addWidget(sum_grp)

        # Cell table: 15 rows × one column per populated segment. Column
        # count follows the target's reported cell_count (60-cell packs show
        # 4 columns, 75-cell show 5); rebuilt in refresh() when it changes.
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
            [f"Cells {i*15}–{i*15+14}" for i in range(n)])

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

        if cs.timestamp_ms:
            self._ts_lbl.setText(f"Snapshot: {cs.timestamp_ms} ms")

        invalid_count = cs.cell_count - len(valid_mv)
        self._warn_lbl.setText(
            f"⚠ {invalid_count} invalid cells" if invalid_count else "")

        # Size the grid to the active pack (multiple of 15).
        self._set_segments((cs.cell_count + 14) // 15 if cs.cell_count else 5)

        for idx, v in enumerate(mv[:cs.cell_count]):
            row = idx % 15
            col = idx // 15
            is_valid = (cs.validity is None or
                        (idx < len(cs.validity) and cs.validity[idx]))
            item = QTableWidgetItem(f"[{idx:02d}] {v}")
            item.setTextAlignment(Qt.AlignmentFlag.AlignCenter)
            if not is_valid:
                item.setBackground(_INVALID_COLOR)
                item.setForeground(_INVALID_TEXT)
            self._table.setItem(row, col, item)

        self._table.resizeColumnsToContents()
