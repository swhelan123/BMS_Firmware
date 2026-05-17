"""config.py — Grouped editable RAM-config editor with validation UX.

Tab layout:
  Editor  — grouped QSpinBox/QLineEdit fields for all user-configurable params
  Raw     — read-only text dump, updated from editor state

Dirty tracking:
  "No config loaded" → "Modified — not applied" → "Validated" → "Applied to RAM"

Store to Flash is NOT implemented; a passive note says so.
"""
import re
from pathlib import Path
from typing import Optional

from PyQt6.QtWidgets import (
    QWidget, QVBoxLayout, QHBoxLayout, QPushButton,
    QLabel, QTextEdit, QFileDialog, QGroupBox, QFormLayout,
    QSpinBox, QLineEdit, QTabWidget, QScrollArea, QFrame,
)
from PyQt6.QtCore import Qt
from PyQt6.QtGui import QColor

from ...core.app_state import AppState
from ...core.target_model import TargetModel, TargetRefusedError
from ...config.schema import BmsConfig
from ...config.validator import validate_config
from ...protocol.client import ProtocolError

_MASK_RE = re.compile(r'^[0-9a-fA-F]{20}$')

# ── Field descriptor helpers ──────────────────────────────────────────────────

def _spin(parent, lo: int, hi: int, step: int = 1) -> QSpinBox:
    w = QSpinBox(parent)
    w.setRange(lo, hi)
    w.setSingleStep(step)
    w.setMaximumWidth(140)
    return w

def _mask_edit(parent) -> QLineEdit:
    e = QLineEdit(parent)
    e.setMaximumWidth(220)
    e.setPlaceholderText("20 hex chars (10 bytes)")
    e.setStyleSheet("font-family: monospace;")
    return e

def _ro_label(text: str = "—") -> QLabel:
    lbl = QLabel(text)
    lbl.setStyleSheet("color:#555; font-family:monospace;")
    return lbl


# ── Config page ───────────────────────────────────────────────────────────────

class ConfigPage(QWidget):
    def __init__(self, state: AppState, main_window, parent=None):
        super().__init__(parent)
        self._state  = state
        self._main   = main_window
        self._cfg:   Optional[BmsConfig] = None
        self._dirty  = False
        self._build_ui()

    # ── UI construction ───────────────────────────────────────────────────────

    def _build_ui(self) -> None:
        outer = QVBoxLayout(self)
        outer.setContentsMargins(8, 8, 8, 8)
        outer.setSpacing(6)

        # ── Action bar ────────────────────────────────────────────────────────
        act_grp = QGroupBox("Actions")
        act_lay = QHBoxLayout(act_grp)
        act_lay.setSpacing(6)

        self._read_btn   = QPushButton("Read from Target")
        self._load_btn   = QPushButton("Load File…")
        self._save_btn   = QPushButton("Save to File…")
        self._export_btn = QPushButton("Export Default Config")
        self._val_btn    = QPushButton("Validate Offline")
        self._apply_btn  = QPushButton("Apply to RAM")

        self._read_btn.setToolTip("Read live config from target (BMS_APP required)")
        self._val_btn.setToolTip("Validate current editor values without connecting")
        self._apply_btn.setToolTip(
            "Validate and send config to target RAM (BMS_APP required)")

        for btn in (self._read_btn, self._load_btn, self._save_btn,
                    self._export_btn, self._val_btn, self._apply_btn):
            act_lay.addWidget(btn)
        act_lay.addStretch()
        outer.addWidget(act_grp)

        self._read_btn.clicked.connect(   self._on_read)
        self._load_btn.clicked.connect(   self._on_load)
        self._save_btn.clicked.connect(   self._on_save)
        self._export_btn.clicked.connect( self._on_export_default)
        self._val_btn.clicked.connect(    self._on_validate_offline)
        self._apply_btn.clicked.connect(  self._on_apply_ram)

        # ── Status bar ────────────────────────────────────────────────────────
        self._status_lbl = QLabel("No config loaded.")
        self._status_lbl.setWordWrap(True)
        self._status_lbl.setStyleSheet("color:#444444; padding:2px 0;")
        outer.addWidget(self._status_lbl)

        # ── Tabs: Editor / Raw ────────────────────────────────────────────────
        self._tabs = QTabWidget()
        outer.addWidget(self._tabs, 1)

        self._tabs.addTab(self._build_editor_tab(), "Editor")
        self._tabs.addTab(self._build_raw_tab(),    "Raw View")

        # ── Flash note ────────────────────────────────────────────────────────
        note = QLabel(
            "ℹ  Persistent config storage (Store to Flash) is not yet implemented. "
            "Apply to RAM only — config reverts to stored flash values on reboot.")
        note.setWordWrap(True)
        note.setStyleSheet(
            "color:#555; font-style:italic; font-size:11px; "
            "padding:4px 6px; background:#f0f0f0; border-radius:3px;")
        outer.addWidget(note)

    def _build_editor_tab(self) -> QWidget:
        scroll = QScrollArea()
        scroll.setWidgetResizable(True)
        inner  = QWidget()
        layout = QVBoxLayout(inner)
        layout.setContentsMargins(8, 8, 8, 8)
        layout.setSpacing(6)
        scroll.setWidget(inner)

        # ── Info (read-only) ──────────────────────────────────────────────────
        info_grp = QGroupBox("Pack / Header (read-only)")
        info_form = QFormLayout(info_grp)
        self._ro_hw_profile  = _ro_label()
        self._ro_schema_ver  = _ro_label()
        self._ro_cell_count  = _ro_label()
        self._ro_temp_count  = _ro_label()
        info_form.addRow("HW Profile ID:",    self._ro_hw_profile)
        info_form.addRow("Schema Version:",   self._ro_schema_ver)
        info_form.addRow("Cell Count:",       self._ro_cell_count)
        info_form.addRow("Temp Count:",       self._ro_temp_count)
        layout.addWidget(info_grp)

        # ── Cell Voltage Thresholds ───────────────────────────────────────────
        cv_grp  = QGroupBox("Cell Voltage Thresholds")
        cv_form = QFormLayout(cv_grp)
        self._f = {}   # field_name → widget (populated below)

        def sp(lo, hi, step=1):
            return _spin(cv_grp, lo, hi, step)

        self._f['cell_uv_hard_mv']          = sp(1000, 5000)
        self._f['cell_uv_soft_mv']          = sp(1000, 5000)
        self._f['cell_ov_soft_mv']          = sp(1000, 5000)
        self._f['cell_ov_hard_mv']          = sp(1000, 5000)
        self._f['cell_nominal_mv']          = sp(1000, 5000)

        cv_form.addRow("UV Hard (mV):",      self._f['cell_uv_hard_mv'])
        cv_form.addRow("UV Soft (mV):",      self._f['cell_uv_soft_mv'])
        cv_form.addRow("OV Soft (mV):",      self._f['cell_ov_soft_mv'])
        cv_form.addRow("OV Hard (mV):",      self._f['cell_ov_hard_mv'])
        cv_form.addRow("Nominal (mV):",      self._f['cell_nominal_mv'])
        layout.addWidget(cv_grp)

        # ── Balancing ─────────────────────────────────────────────────────────
        bal_grp  = QGroupBox("Balancing")
        bal_form = QFormLayout(bal_grp)

        self._f['cell_balance_target_mv']      = _spin(bal_grp, 1000, 5000)
        self._f['cell_balance_hysteresis_mv']  = _spin(bal_grp, 0, 500)
        self._f['balance_on_time_ms']          = _spin(bal_grp, 1, 60000, 100)
        self._f['balance_off_time_ms']         = _spin(bal_grp, 1, 60000, 100)

        bal_form.addRow("Target (mV):",        self._f['cell_balance_target_mv'])
        bal_form.addRow("Hysteresis (mV):",    self._f['cell_balance_hysteresis_mv'])
        bal_form.addRow("On Time (ms):",       self._f['balance_on_time_ms'])
        bal_form.addRow("Off Time (ms):",      self._f['balance_off_time_ms'])
        layout.addWidget(bal_grp)

        # ── Temperature Thresholds ────────────────────────────────────────────
        tmp_grp  = QGroupBox("Temperature Thresholds  (deci-°C, e.g. 450 = 45.0 °C)")
        tmp_form = QFormLayout(tmp_grp)

        def tsp(parent):
            return _spin(parent, -600, 1000)

        self._f['temp_charge_warn_cx10']         = tsp(tmp_grp)
        self._f['temp_charge_hard_cx10']         = tsp(tmp_grp)
        self._f['temp_discharge_warn_cx10']      = tsp(tmp_grp)
        self._f['temp_discharge_hard_cx10']      = tsp(tmp_grp)
        self._f['temp_hard_abs_cx10']            = tsp(tmp_grp)
        self._f['temp_cold_charge_limit_cx10']   = tsp(tmp_grp)
        self._f['temp_cold_discharge_limit_cx10']= tsp(tmp_grp)

        tmp_form.addRow("Charge Warn (deci-°C):",     self._f['temp_charge_warn_cx10'])
        tmp_form.addRow("Charge Hard (deci-°C):",     self._f['temp_charge_hard_cx10'])
        tmp_form.addRow("Discharge Warn (deci-°C):",  self._f['temp_discharge_warn_cx10'])
        tmp_form.addRow("Discharge Hard (deci-°C):",  self._f['temp_discharge_hard_cx10'])
        tmp_form.addRow("Absolute Max (deci-°C):",    self._f['temp_hard_abs_cx10'])
        tmp_form.addRow("Cold Charge Limit (deci-°C):", self._f['temp_cold_charge_limit_cx10'])
        tmp_form.addRow("Cold Discharge Limit (deci-°C):", self._f['temp_cold_discharge_limit_cx10'])
        layout.addWidget(tmp_grp)

        # ── Current / Precharge ───────────────────────────────────────────────
        cur_grp  = QGroupBox("Current & Precharge")
        cur_form = QFormLayout(cur_grp)

        self._f['overcurrent_hard_ma']     = _spin(cur_grp, 1, 2000000, 1000)
        self._f['overcurrent_warn_ma']     = _spin(cur_grp, 0, 2000000, 1000)
        self._f['precharge_pct']           = _spin(cur_grp, 50, 99)
        self._f['precharge_timeout_ms']    = _spin(cur_grp, 1, 300000, 1000)
        self._f['precharge_delta_max_pct'] = _spin(cur_grp, 1, 20)

        cur_form.addRow("Overcurrent Hard (mA):",  self._f['overcurrent_hard_ma'])
        cur_form.addRow("Overcurrent Warn (mA):",  self._f['overcurrent_warn_ma'])
        cur_form.addRow("Precharge Target (%):",   self._f['precharge_pct'])
        cur_form.addRow("Precharge Timeout (ms):", self._f['precharge_timeout_ms'])
        cur_form.addRow("Precharge Delta Max (%):", self._f['precharge_delta_max_pct'])
        layout.addWidget(cur_grp)

        # ── Timing ────────────────────────────────────────────────────────────
        tim_grp  = QGroupBox("Timing")
        tim_form = QFormLayout(tim_grp)

        self._f['temp_settle_time_ms']   = _spin(tim_grp, 1, 2000)
        self._f['stale_data_timeout_ms'] = _spin(tim_grp, 100, 10000, 100)

        tim_form.addRow("Temp Settle (ms):",     self._f['temp_settle_time_ms'])
        tim_form.addRow("Stale Data Timeout (ms):", self._f['stale_data_timeout_ms'])
        layout.addWidget(tim_grp)

        # ── Calibration ───────────────────────────────────────────────────────
        cal_grp  = QGroupBox("Calibration  (gain x1000 = 1000 → ×1.000)")
        cal_form = QFormLayout(cal_grp)

        self._f['vpack_gain_x1000']    = _spin(cal_grp, 1, 10000)
        self._f['vpack_offset_mv']     = _spin(cal_grp, -50000, 50000)
        self._f['vbat_gain_x1000']     = _spin(cal_grp, 1, 10000)
        self._f['vbat_offset_mv']      = _spin(cal_grp, -5000, 5000)
        self._f['current_gain_x1000']  = _spin(cal_grp, 1, 10000)
        self._f['current_offset_ma']   = _spin(cal_grp, -5000, 5000)

        cal_form.addRow("Vpack Gain (×1000):",  self._f['vpack_gain_x1000'])
        cal_form.addRow("Vpack Offset (mV):",   self._f['vpack_offset_mv'])
        cal_form.addRow("Vbat Gain (×1000):",   self._f['vbat_gain_x1000'])
        cal_form.addRow("Vbat Offset (mV):",    self._f['vbat_offset_mv'])
        cal_form.addRow("Current Gain (×1000):", self._f['current_gain_x1000'])
        cal_form.addRow("Current Offset (mA):", self._f['current_offset_ma'])
        layout.addWidget(cal_grp)

        # ── CAN ───────────────────────────────────────────────────────────────
        can_grp  = QGroupBox("CAN (stub — not hardware-validated)")
        can_form = QFormLayout(can_grp)

        self._f['can_watchdog_timeout_ms'] = _spin(can_grp, 0, 60000, 100)
        self._f['can_base_id']             = _spin(can_grp, 0, 0x7FF)

        can_form.addRow("Watchdog Timeout (ms):", self._f['can_watchdog_timeout_ms'])
        can_form.addRow("Base ID (0x000–0x7FF):", self._f['can_base_id'])
        layout.addWidget(can_grp)

        # ── Masks ─────────────────────────────────────────────────────────────
        msk_grp  = QGroupBox(
            "Bitmasks  (10 bytes / 20 hex chars — bits 0–74 valid, bits 75–79 must be 0)")
        msk_form = QFormLayout(msk_grp)

        self._f['required_cell_mask']  = _mask_edit(msk_grp)
        self._f['required_temp_mask']  = _mask_edit(msk_grp)
        self._f['balance_allowed_mask']= _mask_edit(msk_grp)

        msk_form.addRow("Required Cell Mask:",   self._f['required_cell_mask'])
        msk_form.addRow("Required Temp Mask:",   self._f['required_temp_mask'])
        msk_form.addRow("Balance Allowed Mask:", self._f['balance_allowed_mask'])
        layout.addWidget(msk_grp)

        layout.addStretch()

        # Wire dirty signals
        for name, w in self._f.items():
            if isinstance(w, QSpinBox):
                w.valueChanged.connect(self._mark_dirty)
            elif isinstance(w, QLineEdit):
                w.textChanged.connect(self._mark_dirty)

        return scroll

    def _build_raw_tab(self) -> QWidget:
        w = QWidget()
        lay = QVBoxLayout(w)
        lay.setContentsMargins(4, 4, 4, 4)
        self._raw_text = QTextEdit()
        self._raw_text.setReadOnly(True)
        self._raw_text.setFontFamily("Courier")
        self._raw_text.setFontPointSize(11)
        lay.addWidget(self._raw_text)
        return w

    # ── State management ──────────────────────────────────────────────────────

    def _mark_dirty(self) -> None:
        if not self._dirty:
            self._dirty = True
            self._set_status("Modified — not applied.", kind='modified')

    def _set_status(self, msg: str, kind: str = 'neutral') -> None:
        colors = {
            'ok':       '#1a6b1a',
            'warn':     '#9a6000',
            'error':    '#8a0000',
            'neutral':  '#444444',
            'modified': '#0050a0',
        }
        color = colors.get(kind, '#444444')
        self._status_lbl.setStyleSheet(f"color:{color}; font-weight:bold; padding:2px 0;")
        self._status_lbl.setText(msg)

    # ── Populate editor from BmsConfig ────────────────────────────────────────

    def _cfg_to_widgets(self, cfg: BmsConfig) -> None:
        """Populate all editor fields from cfg.  Suppresses dirty-mark during load."""
        # Block all widget signals to avoid triggering _mark_dirty
        for w in self._f.values():
            w.blockSignals(True)

        # Read-only header labels
        self._ro_hw_profile.setText(f"0x{cfg.hw_profile_id:04X}")
        self._ro_schema_ver.setText(str(cfg.schema_version))
        self._ro_cell_count.setText(str(cfg.cell_count))
        self._ro_temp_count.setText(str(cfg.temp_count))

        # Editable spinboxes
        int_fields = [
            'cell_uv_hard_mv', 'cell_uv_soft_mv', 'cell_ov_soft_mv',
            'cell_ov_hard_mv', 'cell_nominal_mv',
            'cell_balance_target_mv', 'cell_balance_hysteresis_mv',
            'balance_on_time_ms', 'balance_off_time_ms',
            'temp_charge_warn_cx10', 'temp_charge_hard_cx10',
            'temp_discharge_warn_cx10', 'temp_discharge_hard_cx10',
            'temp_hard_abs_cx10', 'temp_cold_charge_limit_cx10',
            'temp_cold_discharge_limit_cx10',
            'overcurrent_hard_ma', 'overcurrent_warn_ma',
            'precharge_pct', 'precharge_timeout_ms', 'precharge_delta_max_pct',
            'temp_settle_time_ms', 'stale_data_timeout_ms',
            'vpack_gain_x1000', 'vpack_offset_mv',
            'vbat_gain_x1000', 'vbat_offset_mv',
            'current_gain_x1000', 'current_offset_ma',
            'can_watchdog_timeout_ms', 'can_base_id',
        ]
        for name in int_fields:
            w = self._f.get(name)
            if isinstance(w, QSpinBox):
                val = getattr(cfg, name, 0)
                # Clamp to widget range
                val = max(w.minimum(), min(w.maximum(), val))
                w.setValue(val)

        # Byte masks → hex strings
        for name in ('required_cell_mask', 'required_temp_mask', 'balance_allowed_mask'):
            w = self._f.get(name)
            if isinstance(w, QLineEdit):
                w.setText(getattr(cfg, name, b'\x00' * 10).hex())

        for w in self._f.values():
            w.blockSignals(False)

        self._dirty = False
        self._update_raw(cfg)

    def _widgets_to_cfg(self) -> BmsConfig:
        """Build a BmsConfig from the current editor field values."""
        base = self._cfg if self._cfg is not None else BmsConfig()
        kw = {}
        int_fields = [
            'cell_uv_hard_mv', 'cell_uv_soft_mv', 'cell_ov_soft_mv',
            'cell_ov_hard_mv', 'cell_nominal_mv',
            'cell_balance_target_mv', 'cell_balance_hysteresis_mv',
            'balance_on_time_ms', 'balance_off_time_ms',
            'temp_charge_warn_cx10', 'temp_charge_hard_cx10',
            'temp_discharge_warn_cx10', 'temp_discharge_hard_cx10',
            'temp_hard_abs_cx10', 'temp_cold_charge_limit_cx10',
            'temp_cold_discharge_limit_cx10',
            'overcurrent_hard_ma', 'overcurrent_warn_ma',
            'precharge_pct', 'precharge_timeout_ms', 'precharge_delta_max_pct',
            'temp_settle_time_ms', 'stale_data_timeout_ms',
            'vpack_gain_x1000', 'vpack_offset_mv',
            'vbat_gain_x1000', 'vbat_offset_mv',
            'current_gain_x1000', 'current_offset_ma',
            'can_watchdog_timeout_ms', 'can_base_id',
        ]
        for name in int_fields:
            w = self._f.get(name)
            if isinstance(w, QSpinBox):
                kw[name] = w.value()

        for name in ('required_cell_mask', 'required_temp_mask', 'balance_allowed_mask'):
            w = self._f.get(name)
            if isinstance(w, QLineEdit):
                hex_str = w.text().strip()
                try:
                    kw[name] = bytes.fromhex(hex_str)
                except ValueError:
                    kw[name] = getattr(base, name)

        # Preserve non-editable header fields from loaded config or defaults
        return BmsConfig(
            magic=base.magic,
            schema_version=base.schema_version,
            total_length=base.total_length,
            hw_profile_id=base.hw_profile_id,
            config_generation=base.config_generation,
            config_crc32=0,           # recomputed by pack()
            reserved_header=base.reserved_header,
            cell_count=base.cell_count,
            temp_count=base.temp_count,
            reserved_topology=0,
            reserved_cell_thresholds=0,
            reserved_temp_thresholds=0,
            reserved_temp_params=0,
            can_watchdog_timeout_ms=kw.get('can_watchdog_timeout_ms', base.can_watchdog_timeout_ms),
            can_base_id=kw.get('can_base_id', base.can_base_id),
            reserved_can=0,
            reserved=base.reserved,
            **{k: v for k, v in kw.items()
               if k not in ('can_watchdog_timeout_ms', 'can_base_id')},
        )

    def _update_raw(self, cfg: Optional[BmsConfig]) -> None:
        if cfg is None:
            self._raw_text.setPlainText("")
            return
        lines = [f"# BmsConfig — {len(cfg.pack())} bytes"]
        for name in cfg.__dataclass_fields__:
            v = getattr(cfg, name)
            if isinstance(v, bytes):
                v = v.hex()
            lines.append(f"{name}: {v}")
        self._raw_text.setPlainText('\n'.join(lines))

    # ── Validation helpers ────────────────────────────────────────────────────

    def _validate_masks(self) -> Optional[str]:
        """Return error string if any mask field is invalid, else None."""
        for name in ('required_cell_mask', 'required_temp_mask', 'balance_allowed_mask'):
            w = self._f.get(name)
            if not isinstance(w, QLineEdit):
                continue
            txt = w.text().strip()
            if not _MASK_RE.match(txt):
                return f"{name}: must be exactly 20 hex characters"
            b = bytes.fromhex(txt)
            if b[9] & 0xF8:
                return f"{name}: bits 75–79 (high nibble of byte 9) must be zero"
        return None

    def _highlight_mask_errors(self) -> None:
        for name in ('required_cell_mask', 'required_temp_mask', 'balance_allowed_mask'):
            w = self._f.get(name)
            if not isinstance(w, QLineEdit):
                continue
            txt = w.text().strip()
            bad = not _MASK_RE.match(txt)
            if not bad:
                try:
                    b = bytes.fromhex(txt)
                    bad = bool(b[9] & 0xF8)
                except ValueError:
                    bad = True
            w.setStyleSheet(
                "font-family:monospace; background:#ffe0e0;" if bad
                else "font-family:monospace;")

    # ── refresh() — called on every AppState change ───────────────────────────

    def refresh(self, state: AppState) -> None:
        from ...connection.device_state import DeviceMode
        is_app = (state.device.mode == DeviceMode.BMS_APP)
        self._read_btn.setEnabled(is_app)
        self._apply_btn.setEnabled(is_app and self._cfg is not None)

    # ── Action handlers ───────────────────────────────────────────────────────

    def _on_read(self) -> None:
        model: TargetModel = getattr(self._main, '_model', None)
        if model is None:
            return
        try:
            self._cfg = model.read_config()
            self._cfg_to_widgets(self._cfg)
            self._set_status("Config read from target.", kind='ok')
        except (ProtocolError, TargetRefusedError) as e:
            self._set_status(str(e), kind='error')

    def _on_load(self) -> None:
        path, _ = QFileDialog.getOpenFileName(
            self, "Load Config", "",
            "Config (*.bin *.cfg);;JSON (*.json);;All (*)")
        if not path:
            return
        try:
            if path.endswith('.json'):
                import json
                from ...config.schema import BmsConfig as _BC
                data = json.loads(Path(path).read_text())
                cfg = _BC()
                for k, v in data.items():
                    if hasattr(cfg, k):
                        if isinstance(getattr(cfg, k), bytes):
                            setattr(cfg, k, bytes.fromhex(v) if isinstance(v, str) else v)
                        else:
                            setattr(cfg, k, v)
                self._cfg = cfg
            else:
                self._cfg = BmsConfig.unpack(Path(path).read_bytes())
            self._cfg_to_widgets(self._cfg)
            self._set_status(f"Loaded: {Path(path).name}", kind='ok')
        except Exception as e:
            self._set_status(str(e), kind='error')

    def _on_save(self) -> None:
        if self._cfg is None:
            self._set_status("No config to save.", kind='warn')
            return
        path, _ = QFileDialog.getSaveFileName(
            self, "Save Config", "bms_config.bin",
            "Config (*.bin);;All (*)")
        if not path:
            return
        try:
            cfg = self._widgets_to_cfg()
            Path(path).write_bytes(cfg.pack())
            self._set_status(f"Saved to {Path(path).name}", kind='ok')
        except Exception as e:
            self._set_status(str(e), kind='error')

    def _on_export_default(self) -> None:
        path, _ = QFileDialog.getSaveFileName(
            self, "Export Default Config", "bms_config_default.bin",
            "Config (*.bin);;All (*)")
        if not path:
            return
        try:
            cfg = BmsConfig()
            Path(path).write_bytes(cfg.pack())
            self._cfg = cfg
            self._cfg_to_widgets(self._cfg)
            self._set_status(f"Default config exported to {Path(path).name}", kind='ok')
        except Exception as e:
            self._set_status(str(e), kind='error')

    def _on_validate_offline(self) -> None:
        mask_err = self._validate_masks()
        self._highlight_mask_errors()
        if mask_err:
            self._set_status(f"Validation FAIL — {mask_err}", kind='error')
            return
        try:
            cfg = self._widgets_to_cfg()
        except Exception as e:
            self._set_status(f"Cannot build config: {e}", kind='error')
            return
        ok, err_off, msg = validate_config(cfg)
        if ok:
            self._dirty = False
            self._set_status("Offline validation: PASS", kind='ok')
            self._update_raw(cfg)
        else:
            self._set_status(
                f"Validation FAIL at offset 0x{err_off:04X} — {msg}", kind='error')

    def _on_apply_ram(self) -> None:
        if self._cfg is None:
            self._set_status("No config loaded.", kind='warn')
            return
        mask_err = self._validate_masks()
        self._highlight_mask_errors()
        if mask_err:
            self._set_status(
                f"Apply refused — {mask_err}", kind='error')
            return
        try:
            cfg = self._widgets_to_cfg()
        except Exception as e:
            self._set_status(f"Cannot build config: {e}", kind='error')
            return
        ok_v, err_off, msg_v = validate_config(cfg)
        if not ok_v:
            self._set_status(
                f"Apply refused — local validation FAIL at 0x{err_off:04X}: {msg_v}",
                kind='error')
            return

        model: TargetModel = getattr(self._main, '_model', None)
        if model is None:
            self._set_status("Not connected.", kind='warn')
            return
        try:
            ok_r, err_off_r, msg_r = model.apply_config_ram(cfg)
            if ok_r:
                self._cfg = cfg
                self._dirty = False
                self._set_status("Applied to RAM.", kind='ok')
                self._update_raw(cfg)
            else:
                self._set_status(
                    f"Apply RAM: FAIL at 0x{err_off_r:04X} — {msg_r}", kind='error')
        except (ProtocolError, TargetRefusedError) as e:
            self._set_status(str(e), kind='error')
