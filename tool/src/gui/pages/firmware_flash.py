"""firmware_flash.py — Package selection, inspection, dry-run, execute, and protocol simulation."""
import threading
from pathlib import Path

from PyQt6.QtWidgets import (
    QWidget, QVBoxLayout, QHBoxLayout, QPushButton,
    QLabel, QTextEdit, QFileDialog, QGroupBox, QCheckBox, QProgressBar,
)
from PyQt6.QtCore import pyqtSignal, QObject

from ...core.app_state import AppState
from ...core.target_model import TargetRefusedError
from ...protocol.client import ProtocolError
from ...update.stlink import dry_run_app, detect_programmer
from ...update.package_parser import parse_and_validate_package, PackageValidationError
from ...connection.device_state import DeviceMode


class _SimSignals(QObject):
    progress = pyqtSignal(int, int)  # chunks_done, total
    log      = pyqtSignal(str)
    done     = pyqtSignal(bool, str)  # success, message


class FirmwareFlashPage(QWidget):
    def __init__(self, state: AppState, main_window, parent=None):
        super().__init__(parent)
        self._state    = state
        self._main     = main_window
        self._pkg_path = None
        self._sim_sigs = _SimSignals()
        self._build_ui()
        self._sim_sigs.progress.connect(self._on_sim_progress)
        self._sim_sigs.log.connect(self._append)
        self._sim_sigs.done.connect(self._on_sim_done)

    def _build_ui(self) -> None:
        layout = QVBoxLayout(self)

        # ── Package selection ─────────────────────────────────────────────────
        sel_grp = QGroupBox("Package Selection")
        sel_lay = QHBoxLayout(sel_grp)
        self._path_lbl   = QLabel("No file selected.")
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

        # ── Actions ───────────────────────────────────────────────────────────
        act_grp = QGroupBox("Actions")
        act_lay = QHBoxLayout(act_grp)
        self._inspect_btn  = QPushButton("Inspect")
        self._validate_btn = QPushButton("Validate")
        self._dry_run_btn  = QPushButton("ST-Link Dry Run")
        for btn in (self._inspect_btn, self._validate_btn, self._dry_run_btn):
            act_lay.addWidget(btn)
        layout.addWidget(act_grp)

        self._inspect_btn.clicked.connect( self._on_inspect)
        self._validate_btn.clicked.connect(self._on_validate)
        self._dry_run_btn.clicked.connect( self._on_dry_run)

        # ── Protocol Update Simulation ────────────────────────────────────────
        sim_grp = QGroupBox("Protocol Update Simulation (fake target / pre-hardware)")
        sim_lay = QVBoxLayout(sim_grp)

        sim_note = QLabel(
            "Simulates the OTA update protocol against the fake target.  "
            "Enter Bootloader first, then run the simulation to test begin/chunk/finalize.")
        sim_note.setWordWrap(True)
        sim_note.setStyleSheet("color: #555;")
        sim_lay.addWidget(sim_note)

        sim_btn_lay = QHBoxLayout()
        self._enter_bl_btn  = QPushButton("Enter Bootloader")
        self._run_sim_btn   = QPushButton("Run Simulation")
        self._abort_sim_btn = QPushButton("Abort Update")
        self._abort_sim_btn.setEnabled(False)
        for btn in (self._enter_bl_btn, self._run_sim_btn, self._abort_sim_btn):
            sim_btn_lay.addWidget(btn)
        sim_btn_lay.addStretch()
        sim_lay.addLayout(sim_btn_lay)

        self._sim_progress = QProgressBar()
        self._sim_progress.setVisible(False)
        sim_lay.addWidget(self._sim_progress)

        layout.addWidget(sim_grp)

        self._enter_bl_btn.clicked.connect( self._on_enter_bootloader)
        self._run_sim_btn.clicked.connect(  self._on_run_simulation)
        self._abort_sim_btn.clicked.connect(self._on_abort_sim)

        # ── Execute gate ──────────────────────────────────────────────────────
        exec_grp = QGroupBox("Execute Flash (Hardware Required)")
        exec_lay = QVBoxLayout(exec_grp)
        self._safety_check = QCheckBox(
            "I understand this will flash real hardware via ST-Link")
        self._execute_btn = QPushButton("Flash Hardware")
        self._execute_btn.setEnabled(False)
        self._execute_btn.setStyleSheet("background-color: #c00; color: white;")
        self._safety_check.toggled.connect(self._execute_btn.setEnabled)
        exec_lay.addWidget(self._safety_check)
        exec_lay.addWidget(self._execute_btn)
        layout.addWidget(exec_grp)

        self._execute_btn.clicked.connect(self._on_execute)
        layout.addStretch()

    def refresh(self, state: AppState) -> None:
        is_app = (state.device.mode == DeviceMode.BMS_APP)
        is_bl  = (state.device.mode == DeviceMode.BOOTLOADER)
        self._enter_bl_btn.setEnabled(is_app)
        self._run_sim_btn.setEnabled(is_bl and bool(self._pkg_path))

    def _append(self, text: str) -> None:
        self._info.append(text)

    def _clear(self) -> None:
        self._info.clear()

    # ── Package actions ───────────────────────────────────────────────────────

    def _on_browse(self) -> None:
        path, _ = QFileDialog.getOpenFileName(
            self, "Select Package", "", "Package (*.pkg *.bin);;All (*)")
        if path:
            self._pkg_path = path
            self._path_lbl.setText(path)
            self._on_inspect()
            # Update run-sim button state
            model = getattr(self._main, '_model', None)
            if model and model.device.mode == DeviceMode.BOOTLOADER:
                self._run_sim_btn.setEnabled(True)

    def _on_inspect(self) -> None:
        if not self._pkg_path:
            return
        self._clear()
        try:
            from ...update.package_parser import parse_header
            from ...update.package_builder import PKG_HEADER_SIZE
            raw = Path(self._pkg_path).read_bytes()
            hdr = parse_header(raw[:PKG_HEADER_SIZE])
            self._append(f"Magic:      0x{hdr.pkg_magic:08X}")
            self._append(f"Version:    {'.'.join(str(x) for x in hdr.fw_version)}")
            self._append(f"HW Profile: 0x{hdr.hw_profile_id:04X}")
            self._append(f"App addr:   0x{hdr.app_start_addr:08X}")
            self._append(f"App size:   {hdr.app_size} bytes")
            self._append(f"App CRC:    0x{hdr.app_crc32:08X}")
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

    # ── Protocol simulation ───────────────────────────────────────────────────

    def _on_enter_bootloader(self) -> None:
        model = getattr(self._main, '_model', None)
        if model is None:
            return
        self._clear()
        try:
            model.enter_bootloader()
            # Re-run capabilities handshake so device state reflects BOOTLOADER
            device = model.capabilities_handshake()
            state_obj = getattr(self._main, '_state', None)
            if state_obj:
                state_obj.update_device(device)
            self._append(f"Entered bootloader — mode: {device.mode.name}")
        except (TargetRefusedError, ProtocolError) as e:
            self._append(f"Error: {e}")

    def _on_run_simulation(self) -> None:
        model = getattr(self._main, '_model', None)
        if model is None or not self._pkg_path:
            return
        self._clear()
        self._run_sim_btn.setEnabled(False)
        self._abort_sim_btn.setEnabled(True)
        self._sim_progress.setVisible(True)
        self._sim_progress.setValue(0)
        self._abort_flag = threading.Event()

        def _worker():
            try:
                from ...update.package_parser import parse_and_validate_package
                hdr, payload = parse_and_validate_package(self._pkg_path)

                from ...update.package_builder import PKG_HEADER_SIZE
                raw = Path(self._pkg_path).read_bytes()
                header_bytes = raw[:PKG_HEADER_SIZE]

                self._sim_sigs.log.emit("begin_update …")
                resp = model.boot_update_begin(header_bytes)
                if resp['result'] != 0:
                    self._sim_sigs.done.emit(
                        False,
                        f"begin refused: result={resp['result']} "
                        f"reason={resp['reject_reason']}")
                    return

                chunk_size   = resp['expected_chunk_size']
                total_chunks = resp['total_chunks']
                self._sim_sigs.log.emit(
                    f"chunk_size={chunk_size}  total_chunks={total_chunks}")
                self._sim_progress.setMaximum(total_chunks)

                for i in range(total_chunks):
                    if self._abort_flag.is_set():
                        model.boot_update_abort()
                        self._sim_sigs.done.emit(False, "Aborted by user.")
                        return
                    start = i * chunk_size
                    chunk = payload[start:start + chunk_size]
                    rc = model.boot_update_chunk(i, chunk)
                    if rc != 0:
                        self._sim_sigs.done.emit(
                            False, f"chunk {i} rejected: rc={rc}")
                        return
                    self._sim_sigs.progress.emit(i + 1, total_chunks)

                self._sim_sigs.log.emit("finalizing …")
                fin = model.boot_update_finalize()
                ok  = (fin['result'] == 0)
                self._sim_sigs.done.emit(
                    ok,
                    f"Finalize: result={fin['result']}  "
                    f"crc=0x{fin['computed_crc']:08X}")
            except PackageValidationError as e:
                self._sim_sigs.done.emit(False, f"Package invalid: {e}")
            except (TargetRefusedError, ProtocolError) as e:
                self._sim_sigs.done.emit(False, f"Protocol error: {e}")
            except Exception as e:
                self._sim_sigs.done.emit(False, f"Error: {e}")

        threading.Thread(target=_worker, daemon=True).start()

    def _on_abort_sim(self) -> None:
        if hasattr(self, '_abort_flag'):
            self._abort_flag.set()

    def _on_sim_progress(self, done: int, total: int) -> None:
        self._sim_progress.setMaximum(total)
        self._sim_progress.setValue(done)

    def _on_sim_done(self, success: bool, msg: str) -> None:
        self._append(f"{'✓' if success else '✗'} {msg}")
        self._run_sim_btn.setEnabled(True)
        self._abort_sim_btn.setEnabled(False)
        self._sim_progress.setVisible(False)

    # ── Hardware execute ──────────────────────────────────────────────────────

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
