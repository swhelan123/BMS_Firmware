"""connection.py — Connection page: connect to TCP fake target or serial port."""
from PyQt6.QtWidgets import (
    QWidget, QVBoxLayout, QHBoxLayout, QGroupBox,
    QLabel, QLineEdit, QPushButton, QComboBox, QSpinBox,
)
from PyQt6.QtCore import pyqtSignal

from ...core.app_state import AppState
from ...connection.device_state import DeviceMode


class ConnectionPage(QWidget):
    connect_requested    = pyqtSignal(str, int)  # host, port
    disconnect_requested = pyqtSignal()

    def __init__(self, state: AppState, parent=None):
        super().__init__(parent)
        self._state = state
        self._build_ui()

    def _build_ui(self) -> None:
        layout = QVBoxLayout(self)

        # ── TCP group ────────────────────────────────────────────────────────
        tcp_grp = QGroupBox("TCP Connection (fake target)")
        tcp_lay = QHBoxLayout(tcp_grp)

        tcp_lay.addWidget(QLabel("Host:"))
        self._host_edit = QLineEdit("127.0.0.1")
        self._host_edit.setMaximumWidth(150)
        tcp_lay.addWidget(self._host_edit)

        tcp_lay.addWidget(QLabel("Port:"))
        self._port_spin = QSpinBox()
        self._port_spin.setRange(1, 65535)
        self._port_spin.setValue(65102)
        self._port_spin.setMaximumWidth(80)
        tcp_lay.addWidget(self._port_spin)

        self._connect_btn = QPushButton("Connect")
        self._connect_btn.clicked.connect(self._on_connect)
        tcp_lay.addWidget(self._connect_btn)

        self._disconnect_btn = QPushButton("Disconnect")
        self._disconnect_btn.clicked.connect(self.disconnect_requested)
        self._disconnect_btn.setEnabled(False)
        tcp_lay.addWidget(self._disconnect_btn)

        tcp_lay.addStretch()
        layout.addWidget(tcp_grp)

        # ── Status group ─────────────────────────────────────────────────────
        status_grp = QGroupBox("Device Status")
        status_lay = QVBoxLayout(status_grp)

        self._mode_label   = QLabel("Mode: —")
        self._fw_label     = QLabel("Firmware: —")
        self._hw_label     = QLabel("HW Profile: —")
        self._proto_label  = QLabel("Protocol: —")
        self._cells_label  = QLabel("Cells/Temps: —")
        self._error_label  = QLabel("")
        self._error_label.setStyleSheet("color: red;")

        for w in (self._mode_label, self._fw_label, self._hw_label,
                  self._proto_label, self._cells_label, self._error_label):
            status_lay.addWidget(w)

        layout.addWidget(status_grp)
        layout.addStretch()

    def _on_connect(self) -> None:
        self.connect_requested.emit(self._host_edit.text(), self._port_spin.value())
        self._connect_btn.setEnabled(False)
        self._disconnect_btn.setEnabled(True)

    def refresh(self, state: AppState) -> None:
        d    = state.device
        caps = d.capabilities
        self._mode_label.setText(f"Mode: {d.mode.name}")
        self._error_label.setText(d.error_msg or "")

        if caps:
            self._fw_label.setText(
                f"Firmware: v{'.'.join(str(x) for x in caps.firmware_version)}  "
                f"(type 0x{caps.firmware_type:04X})")
            self._hw_label.setText(f"HW Profile: 0x{caps.hw_profile_id:04X}")
            self._proto_label.setText(f"Protocol: v{caps.protocol_version}")
            self._cells_label.setText(
                f"Cells: {caps.cell_count}  Temps: {caps.temp_count}")
        else:
            for lbl in (self._fw_label, self._hw_label,
                        self._proto_label, self._cells_label):
                lbl.setText("—")

        connected = d.mode != DeviceMode.DISCONNECTED
        self._connect_btn.setEnabled(not connected)
        self._disconnect_btn.setEnabled(connected)
