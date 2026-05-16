"""logs.py — Packet log and event log viewer."""
from PyQt6.QtWidgets import (
    QWidget, QVBoxLayout, QHBoxLayout, QPushButton,
    QTabWidget, QTextEdit, QGroupBox,
)
from PyQt6.QtCore import Qt

from ...core.logging_model import PacketLog, EventLog


class LogsPage(QWidget):
    def __init__(self, evt_log: EventLog, pkt_log: PacketLog, parent=None):
        super().__init__(parent)
        self._evt_log = evt_log
        self._pkt_log = pkt_log
        self._build_ui()

    def _build_ui(self) -> None:
        layout = QVBoxLayout(self)

        tabs = QTabWidget()

        # Event log tab
        self._evt_text = QTextEdit()
        self._evt_text.setReadOnly(True)
        self._evt_text.setFontFamily("Courier")
        tabs.addTab(self._evt_text, "Events")

        # Packet log tab
        self._pkt_text = QTextEdit()
        self._pkt_text.setReadOnly(True)
        self._pkt_text.setFontFamily("Courier")
        tabs.addTab(self._pkt_text, "Packets")

        layout.addWidget(tabs, 1)

        # Controls
        ctrl = QHBoxLayout()
        self._refresh_btn   = QPushButton("Refresh")
        self._clear_evt_btn = QPushButton("Clear Events")
        self._clear_pkt_btn = QPushButton("Clear Packets")
        self._export_btn    = QPushButton("Export Events…")

        self._refresh_btn.clicked.connect(self.refresh)
        self._clear_evt_btn.clicked.connect(self._on_clear_evt)
        self._clear_pkt_btn.clicked.connect(self._on_clear_pkt)
        self._export_btn.clicked.connect(self._on_export)

        for btn in (self._refresh_btn, self._clear_evt_btn,
                    self._clear_pkt_btn, self._export_btn):
            ctrl.addWidget(btn)
        ctrl.addStretch()
        layout.addLayout(ctrl)

    def refresh(self, *_) -> None:
        self._evt_text.setPlainText(self._evt_log.export_text())
        self._pkt_text.setPlainText(self._pkt_log.export_text())
        # Scroll to bottom
        for w in (self._evt_text, self._pkt_text):
            sb = w.verticalScrollBar()
            if sb:
                sb.setValue(sb.maximum())

    def _on_clear_evt(self) -> None:
        self._evt_log.clear()
        self._evt_text.clear()

    def _on_clear_pkt(self) -> None:
        self._pkt_log.clear()
        self._pkt_text.clear()

    def _on_export(self) -> None:
        from PyQt6.QtWidgets import QFileDialog
        from pathlib import Path
        path, _ = QFileDialog.getSaveFileName(self, "Export Event Log",
                                              "bms_events.txt", "Text (*.txt);;All (*)")
        if path:
            Path(path).write_text(self._evt_log.export_text())
