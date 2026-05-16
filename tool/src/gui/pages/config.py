"""config.py — Config read/load/validate/apply-RAM page."""
from pathlib import Path

from PyQt6.QtWidgets import (
    QWidget, QVBoxLayout, QHBoxLayout, QPushButton,
    QLabel, QTextEdit, QFileDialog, QGroupBox, QMessageBox,
)
from PyQt6.QtCore import Qt

from ...core.app_state import AppState
from ...core.target_model import TargetModel, TargetRefusedError
from ...config.schema import BmsConfig
from ...config.validator import validate_config
from ...protocol.client import ProtocolError


class ConfigPage(QWidget):
    def __init__(self, state: AppState, main_window, parent=None):
        super().__init__(parent)
        self._state  = state
        self._main   = main_window  # for accessing TargetModel
        self._cfg:   BmsConfig = None
        self._build_ui()

    def _build_ui(self) -> None:
        layout = QVBoxLayout(self)

        # Action buttons
        btn_grp = QGroupBox("Actions")
        btn_lay = QHBoxLayout(btn_grp)

        self._read_btn    = QPushButton("Read from Target")
        self._load_btn    = QPushButton("Load File…")
        self._save_btn    = QPushButton("Save to File…")
        self._export_btn  = QPushButton("Export Default Config")
        self._val_btn     = QPushButton("Validate Offline")
        self._apply_btn   = QPushButton("Apply to RAM")
        self._store_btn   = QPushButton("Store to Flash")

        for btn in (self._read_btn, self._load_btn, self._save_btn,
                    self._export_btn, self._val_btn,
                    self._apply_btn,  self._store_btn):
            btn_lay.addWidget(btn)

        self._read_btn.clicked.connect(   self._on_read)
        self._load_btn.clicked.connect(   self._on_load)
        self._save_btn.clicked.connect(   self._on_save)
        self._export_btn.clicked.connect( self._on_export_default)
        self._val_btn.clicked.connect(    self._on_validate_offline)
        self._apply_btn.clicked.connect(  self._on_apply_ram)
        self._store_btn.clicked.connect(  self._on_store)

        layout.addWidget(btn_grp)

        # Validation status
        self._status_lbl = QLabel("No config loaded.")
        self._status_lbl.setWordWrap(True)
        layout.addWidget(self._status_lbl)

        # Config dump (simple text view)
        self._text = QTextEdit()
        self._text.setReadOnly(True)
        self._text.setFontFamily("Courier")
        layout.addWidget(self._text)

    def refresh(self, state: AppState) -> None:
        from ...connection.device_state import DeviceMode
        is_app = (state.device.mode == DeviceMode.BMS_APP)
        self._read_btn.setEnabled(is_app)
        self._apply_btn.setEnabled(is_app and self._cfg is not None)
        self._store_btn.setEnabled(False)  # store not supported in fake target

    def _set_status(self, msg: str, ok: bool = True) -> None:
        color = "green" if ok else "red"
        self._status_lbl.setText(f'<span style="color:{color}">{msg}</span>')

    def _show_config(self) -> None:
        if self._cfg is None:
            return
        lines = []
        for f in self._cfg.__dataclass_fields__:
            v = getattr(self._cfg, f)
            if isinstance(v, bytes):
                v = v.hex()
            lines.append(f"{f}: {v}")
        self._text.setPlainText('\n'.join(lines))

    def _on_read(self) -> None:
        model: TargetModel = getattr(self._main, '_model', None)
        if model is None:
            return
        try:
            self._cfg = model.read_config()
            self._show_config()
            self._set_status("Config read from target.")
        except (ProtocolError, TargetRefusedError) as e:
            self._set_status(str(e), ok=False)

    def _on_load(self) -> None:
        path, _ = QFileDialog.getOpenFileName(self, "Load Config", "", "Config (*.bin *.cfg);;All (*)")
        if not path:
            return
        try:
            self._cfg = BmsConfig.unpack(Path(path).read_bytes())
            self._show_config()
            self._set_status(f"Loaded: {path}")
        except Exception as e:
            self._set_status(str(e), ok=False)

    def _on_save(self) -> None:
        if self._cfg is None:
            self._set_status("No config to save.", ok=False)
            return
        path, _ = QFileDialog.getSaveFileName(self, "Save Config", "bms_config.bin",
                                              "Config (*.bin);;All (*)")
        if not path:
            return
        Path(path).write_bytes(self._cfg.pack())
        self._set_status(f"Saved to {path}")

    def _on_validate_offline(self) -> None:
        if self._cfg is None:
            self._set_status("No config loaded.", ok=False)
            return
        ok, err_off, msg = validate_config(self._cfg)
        if ok:
            self._set_status("Offline validation: PASS")
        else:
            self._set_status(
                f"Offline validation: FAIL at offset 0x{err_off:04X} — {msg}", ok=False)

    def _on_apply_ram(self) -> None:
        if self._cfg is None:
            self._set_status("No config loaded.", ok=False)
            return
        model: TargetModel = getattr(self._main, '_model', None)
        if model is None:
            return
        try:
            ok, err_off, msg = model.apply_config_ram(self._cfg)
            self._set_status(f"Apply RAM: {'PASS' if ok else 'FAIL — ' + msg}", ok=ok)
        except (ProtocolError, TargetRefusedError) as e:
            self._set_status(str(e), ok=False)

    def _on_export_default(self) -> None:
        from ...config.schema import BmsConfig
        path, _ = QFileDialog.getSaveFileName(
            self, "Export Default Config", "bms_config_default.bin",
            "Config (*.bin);;All (*)")
        if not path:
            return
        try:
            cfg = BmsConfig()
            Path(path).write_bytes(cfg.pack())
            self._cfg = cfg
            self._show_config()
            self._set_status(f"Default config exported to {path}")
        except Exception as e:
            self._set_status(str(e), ok=False)

    def _on_store(self) -> None:
        QMessageBox.information(self, "Store Config",
                                "Store to flash is not supported by the fake target.")
