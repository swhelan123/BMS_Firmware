"""firmware_flash.py — Package selection, inspection, dry-run, and execute gate."""
from pathlib import Path

from PyQt6.QtWidgets import (
    QWidget, QVBoxLayout, QHBoxLayout, QPushButton,
    QLabel, QTextEdit, QFileDialog, QGroupBox, QCheckBox,
)

from ...core.app_state import AppState
from ...update.stlink import dry_run_app, detect_programmer
from ...update.package_parser import parse_and_validate_package, PackageValidationError


class FirmwareFlashPage(QWidget):
    def __init__(self, state: AppState, main_window, parent=None):
        super().__init__(parent)
        self._state    = state
        self._main     = main_window
        self._pkg_path = None
        self._build_ui()

    def _build_ui(self) -> None:
        layout = QVBoxLayout(self)

        # Package selection
        sel_grp = QGroupBox("Package Selection")
        sel_lay = QHBoxLayout(sel_grp)
        self._path_lbl  = QLabel("No file selected.")
        self._browse_btn = QPushButton("Browse…")
        self._browse_btn.clicked.connect(self._on_browse)
        sel_lay.addWidget(self._path_lbl, 1)
        sel_lay.addWidget(self._browse_btn)
        layout.addWidget(sel_grp)

        # Package info
        self._info = QTextEdit()
        self._info.setReadOnly(True)
        self._info.setMaximumHeight(150)
        self._info.setFontFamily("Courier")
        layout.addWidget(self._info)

        # Actions
        act_grp = QGroupBox("Actions")
        act_lay = QHBoxLayout(act_grp)
        self._inspect_btn  = QPushButton("Inspect")
        self._validate_btn = QPushButton("Validate")
        self._dry_run_btn  = QPushButton("ST-Link Dry Run")
        for btn in (self._inspect_btn, self._validate_btn, self._dry_run_btn):
            act_lay.addWidget(btn)
        layout.addWidget(act_grp)

        self._inspect_btn.clicked.connect(self._on_inspect)
        self._validate_btn.clicked.connect(self._on_validate)
        self._dry_run_btn.clicked.connect(self._on_dry_run)

        # Execute gate — disabled until user checks the safety checkbox
        exec_grp = QGroupBox("Execute Flash (Hardware Required)")
        exec_lay = QVBoxLayout(exec_grp)
        self._safety_check = QCheckBox(
            "I understand this will flash real hardware via ST-Link")
        self._execute_btn  = QPushButton("Flash Hardware")
        self._execute_btn.setEnabled(False)
        self._execute_btn.setStyleSheet("background-color: #c00; color: white;")
        self._safety_check.toggled.connect(self._execute_btn.setEnabled)
        exec_lay.addWidget(self._safety_check)
        exec_lay.addWidget(self._execute_btn)
        layout.addWidget(exec_grp)

        self._execute_btn.clicked.connect(self._on_execute)
        layout.addStretch()

    def refresh(self, state: AppState) -> None:
        pass  # no per-poll refresh needed for this page

    def _append(self, text: str) -> None:
        self._info.append(text)

    def _clear(self) -> None:
        self._info.clear()

    def _on_browse(self) -> None:
        path, _ = QFileDialog.getOpenFileName(
            self, "Select Package", "", "Package (*.pkg *.bin);;All (*)")
        if path:
            self._pkg_path = path
            self._path_lbl.setText(path)
            self._on_inspect()

    def _on_inspect(self) -> None:
        if not self._pkg_path:
            return
        self._clear()
        try:
            from ...update.package_parser import parse_header
            from ...update.package_builder import PKG_HEADER_SIZE
            raw = Path(self._pkg_path).read_bytes()
            hdr = parse_header(raw[:PKG_HEADER_SIZE])
            self._append(f"Magic:     0x{hdr.pkg_magic:08X}")
            self._append(f"Version:   {'.'.join(str(x) for x in hdr.fw_version)}")
            self._append(f"HW Profile: 0x{hdr.hw_profile_id:04X}")
            self._append(f"App addr:  0x{hdr.app_start_addr:08X}")
            self._append(f"App size:  {hdr.app_size} bytes")
            self._append(f"App CRC:   0x{hdr.app_crc32:08X}")
        except Exception as e:
            self._append(f"Error: {e}")

    def _on_validate(self) -> None:
        if not self._pkg_path:
            return
        self._clear()
        try:
            hdr, payload = parse_and_validate_package(self._pkg_path)
            self._append("Package VALID")
            self._append(f"Firmware: v{'.'.join(str(x) for x in hdr.fw_version)}")
            self._append(f"Payload:  {len(payload)} bytes")
        except PackageValidationError as e:
            self._append(f"INVALID: {e}")
        except FileNotFoundError:
            self._append("File not found")

    def _on_dry_run(self) -> None:
        if not self._pkg_path:
            return
        self._clear()
        try:
            _cmd, status = dry_run_app(self._pkg_path)
            self._append(status)
            if not detect_programmer():
                self._append("\n⚠ STM32_Programmer_CLI not found on PATH.")
        except Exception as e:
            self._append(f"Error: {e}")

    def _on_execute(self) -> None:
        if not self._pkg_path or not self._safety_check.isChecked():
            return
        from ...update.stlink import execute_flash
        self._clear()
        self._append("Executing flash …")
        try:
            rc, output = execute_flash(self._pkg_path, confirm=True)
            self._append(output)
            self._append(f"\nReturn code: {rc}")
        except Exception as e:
            self._append(f"Error: {e}")
        finally:
            self._safety_check.setChecked(False)
