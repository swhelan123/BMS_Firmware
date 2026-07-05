"""connection.py — Connection page: TCP (fake target) or serial port."""
from PyQt6.QtWidgets import (
    QWidget, QVBoxLayout, QHBoxLayout, QGroupBox,
    QLabel, QLineEdit, QPushButton, QComboBox, QSpinBox,
)
from PyQt6.QtCore import pyqtSignal, Qt

from ...core.app_state import AppState
from ..style import MONOSPACE
from ...connection.device_state import DeviceMode


class ConnectionPage(QWidget):
    connect_requested        = pyqtSignal(str, int)   # host, port (TCP)
    connect_serial_requested = pyqtSignal(str, int)   # device, baud
    disconnect_requested     = pyqtSignal()

    def __init__(self, state: AppState, parent=None):
        super().__init__(parent)
        self._state = state
        self._build_ui()

    def _build_ui(self) -> None:
        layout = QVBoxLayout(self)
        layout.setContentsMargins(10, 10, 10, 10)

        # ── TCP group ────────────────────────────────────────────────────────
        tcp_grp = QGroupBox("TCP Connection (fake target / simulator)")
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

        self._connect_tcp_btn = QPushButton("Connect TCP")
        self._connect_tcp_btn.clicked.connect(self._on_connect_tcp)
        tcp_lay.addWidget(self._connect_tcp_btn)
        tcp_lay.addStretch()
        layout.addWidget(tcp_grp)

        # ── Serial group ─────────────────────────────────────────────────────
        ser_grp = QGroupBox("Serial Connection (hardware)")
        ser_lay = QHBoxLayout(ser_grp)

        ser_lay.addWidget(QLabel("Port:"))
        self._serial_combo = QComboBox()
        self._serial_combo.setEditable(True)
        self._serial_combo.setMinimumWidth(220)
        ser_lay.addWidget(self._serial_combo)

        self._rescan_btn = QPushButton("Rescan")
        self._rescan_btn.clicked.connect(self._scan_ports)
        ser_lay.addWidget(self._rescan_btn)
        self._scan_ports()

        ser_lay.addWidget(QLabel("Baud:"))
        self._baud_spin = QSpinBox()
        self._baud_spin.setRange(9600, 921600)
        self._baud_spin.setValue(115200)
        self._baud_spin.setMaximumWidth(90)
        ser_lay.addWidget(self._baud_spin)

        self._connect_ser_btn = QPushButton("Connect Serial")
        self._connect_ser_btn.clicked.connect(self._on_connect_serial)
        ser_lay.addWidget(self._connect_ser_btn)
        ser_lay.addStretch()
        layout.addWidget(ser_grp)

        # ── Disconnect ───────────────────────────────────────────────────────
        dc_lay = QHBoxLayout()
        self._disconnect_btn = QPushButton("Disconnect")
        self._disconnect_btn.clicked.connect(self.disconnect_requested)
        self._disconnect_btn.setEnabled(False)
        dc_lay.addWidget(self._disconnect_btn)
        dc_lay.addStretch()
        layout.addLayout(dc_lay)

        # ── Quick-start hints ─────────────────────────────────────────────────
        hint_grp = QGroupBox("Quick Start — Fake Target Commands")
        hint_lay = QVBoxLayout(hint_grp)
        hint_lay.addWidget(QLabel(
            "Run one of these in a terminal, then connect via TCP (port 65102):"))
        hint_cmds = [
            ("Static healthy (port 65102):",
             "./scripts/bmsctl.sh fake-target run --mode healthy"),
            ("Static open-wire (port 65102):",
             "./scripts/bmsctl.sh fake-target run --mode openwire_detected"),
            ("Live drive simulation (port 65103):",
             "./scripts/run_fake_hardware.sh --mode drive"),
            ("GUI with auto-connect (healthy):",
             "./scripts/run_gui.sh --fake --mode healthy"),
        ]
        for desc, cmd in hint_cmds:
            row = QHBoxLayout()
            desc_lbl = QLabel(desc)
            desc_lbl.setFixedWidth(230)
            cmd_lbl  = QLabel(cmd)
            cmd_lbl.setStyleSheet(f"font-family: {MONOSPACE};")
            cmd_lbl.setTextInteractionFlags(
                Qt.TextInteractionFlag.TextSelectableByMouse)
            row.addWidget(desc_lbl)
            row.addWidget(cmd_lbl)
            row.addStretch()
            hint_lay.addLayout(row)
        layout.addWidget(hint_grp)

        # ── Device status ────────────────────────────────────────────────────
        status_grp = QGroupBox("Device Status")
        status_lay = QVBoxLayout(status_grp)

        self._mode_label  = QLabel("Mode: —")
        self._fw_label    = QLabel("Firmware: —")
        self._hw_label    = QLabel("HW Profile: —")
        self._proto_label = QLabel("Protocol: —")
        self._cells_label = QLabel("Cells / Temps: —")
        self._error_label = QLabel("")
        self._error_label.setStyleSheet("color: #8a0000; font-weight:bold;")

        for w in (self._mode_label, self._fw_label, self._hw_label,
                  self._proto_label, self._cells_label, self._error_label):
            status_lay.addWidget(w)

        layout.addWidget(status_grp)
        layout.addStretch()

    # ── Serial port autodetection ─────────────────────────────────────────────

    def _scan_ports(self) -> None:
        """Populate the port combo with detected serial devices.

        USB-serial adapters (CP2104 on the BMS master) are listed first and
        the best match is pre-selected. Falls back to an empty editable box.
        """
        try:
            from serial.tools import list_ports
            ports = [p.device for p in list_ports.comports()]
        except Exception:
            ports = []

        # Rank: usbserial/usbmodem/SLAB first, Bluetooth/debug ports last
        def rank(dev: str) -> int:
            d = dev.lower()
            if "usbserial" in d or "slab" in d: return 0
            if "usbmodem" in d:                 return 1
            if "bluetooth" in d or "debug" in d: return 3
            return 2
        ports.sort(key=rank)

        current = self._serial_combo.currentText().strip()
        self._serial_combo.clear()
        self._serial_combo.addItems(ports)
        if current and current in ports:
            self._serial_combo.setCurrentText(current)
        elif ports and rank(ports[0]) <= 1:
            self._serial_combo.setCurrentText(ports[0])
        else:
            self._serial_combo.setCurrentText("")

    # ── Slots ─────────────────────────────────────────────────────────────────

    def _on_connect_tcp(self) -> None:
        self.connect_requested.emit(
            self._host_edit.text(), self._port_spin.value())
        self._set_connecting()

    def _on_connect_serial(self) -> None:
        device = self._serial_combo.currentText().strip()
        if not device:
            return
        self.connect_serial_requested.emit(device, self._baud_spin.value())
        self._set_connecting()

    def _set_connecting(self) -> None:
        self._connect_tcp_btn.setEnabled(False)
        self._connect_ser_btn.setEnabled(False)
        self._disconnect_btn.setEnabled(True)

    # ── Refresh ───────────────────────────────────────────────────────────────

    def refresh(self, state: AppState) -> None:
        d    = state.device
        caps = d.capabilities
        self._mode_label.setText(f"Mode: {d.mode.name}")
        self._error_label.setText(d.error_msg or "")

        if caps:
            fw_ver = '.'.join(str(x) for x in caps.firmware_version)
            self._fw_label.setText(
                f"Firmware: v{fw_ver}  (type 0x{caps.firmware_type:04X})")
            self._hw_label.setText(f"HW Profile: 0x{caps.hw_profile_id:04X}")
            self._proto_label.setText(f"Protocol: v{caps.protocol_version}")
            self._cells_label.setText(
                f"Cells: {caps.cell_count}  Temps: {caps.temp_count}")
        else:
            for lbl in (self._fw_label, self._hw_label,
                        self._proto_label, self._cells_label):
                lbl.setText("—")

        connected = d.mode != DeviceMode.DISCONNECTED
        self._connect_tcp_btn.setEnabled(not connected)
        self._connect_ser_btn.setEnabled(not connected)
        self._disconnect_btn.setEnabled(connected)
