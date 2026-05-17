"""bringup.py — Bring-Up / Diagnostics page for bench testing without hardware."""
from PyQt6.QtWidgets import (
    QWidget, QVBoxLayout, QHBoxLayout, QGridLayout, QGroupBox,
    QLabel, QPushButton, QTextEdit, QScrollArea,
)
from PyQt6.QtCore import Qt

from ...core.app_state import AppState
from ...core.target_model import TargetRefusedError
from ...protocol.client import ProtocolError


class BringupPage(QWidget):
    def __init__(self, state: AppState, main_window, parent=None):
        super().__init__(parent)
        self._state = state
        self._main  = main_window
        self._build_ui()

    # ── UI construction ───────────────────────────────────────────────────────

    def _build_ui(self) -> None:
        scroll = QScrollArea()
        scroll.setWidgetResizable(True)
        inner  = QWidget()
        layout = QVBoxLayout(inner)
        layout.setContentsMargins(8, 8, 8, 8)
        layout.setSpacing(8)
        scroll.setWidget(inner)
        outer = QVBoxLayout(self)
        outer.setContentsMargins(0, 0, 0, 0)
        outer.addWidget(scroll)

        # ── Mode notice (shown in non-APP mode) ───────────────────────────────
        self._mode_notice = QLabel(
            "Bring-up actions require BMS_APP mode.")
        self._mode_notice.setStyleSheet(
            "color:#9a6000; font-weight:bold; font-size:12px; "
            "padding:6px 10px; background:#fff8e6; border:1px solid #d4a800; "
            "border-radius:4px;")
        self._mode_notice.setWordWrap(True)
        self._mode_notice.setVisible(False)
        layout.addWidget(self._mode_notice)

        # ── Diagnostics counters ──────────────────────────────────────────────
        diag_grp  = QGroupBox("Diagnostics Counters")
        diag_grid = QGridLayout(diag_grp)
        diag_grid.setColumnMinimumWidth(0, 160)

        def drow(label: str, r: int) -> QLabel:
            diag_grid.addWidget(
                QLabel(label), r, 0, alignment=Qt.AlignmentFlag.AlignRight)
            val = QLabel("—")
            diag_grid.addWidget(val, r, 1)
            return val

        self._reset_cause    = drow("Reset Cause:",       0)
        self._pec_cell_err   = drow("PEC Cell Errors:",   1)
        self._pec_temp_err   = drow("PEC Temp Errors:",   2)
        self._i2c_err        = drow("I2C Errors:",        3)
        self._openwire_valid = drow("Open-Wire Valid:",   4)
        self._openwire_mask  = drow("Open-Wire Mask:",    5)
        self._diag_uptime    = drow("Uptime (s):",        6)

        d_btn_lay = QHBoxLayout()
        self._refresh_diag_btn = QPushButton("Refresh Diagnostics")
        self._refresh_diag_btn.clicked.connect(self._on_refresh_diagnostics)
        d_btn_lay.addWidget(self._refresh_diag_btn)
        d_btn_lay.addStretch()
        diag_grid.addLayout(d_btn_lay, 7, 0, 1, 2)
        layout.addWidget(diag_grp)

        # ── GPIO / Outputs snapshot ───────────────────────────────────────────
        gpio_grp  = QGroupBox("GPIO & Outputs Snapshot")
        gpio_grid = QGridLayout(gpio_grp)
        gpio_grid.setColumnMinimumWidth(0, 160)

        def grow(label: str, r: int) -> QLabel:
            gpio_grid.addWidget(
                QLabel(label), r, 0, alignment=Qt.AlignmentFlag.AlignRight)
            val = QLabel("—")
            gpio_grid.addWidget(val, r, 1)
            return val

        self._cs_cell       = grow("CS_CELL:",              0)
        self._cs_temp       = grow("CS_TEMP:",              1)
        self._power_btn_lbl = grow("Power Button:",         2)
        self._charge_det    = grow("Charge Detect:",        3)
        self._power_en      = grow("Power Enable:",         4)
        self._master_ok     = grow("Master OK (raw):",      5)
        self._discharge_r   = grow("Discharge (raw):",      6)
        self._charge_r      = grow("Charge (raw):",         7)
        self._charger_safe  = grow("Charger Safety (raw):", 8)
        self._out_logical   = grow("Outputs Logical:",      9)
        self._out_raw       = grow("Outputs Raw:",          10)

        g_btn_lay = QHBoxLayout()
        self._refresh_gpio_btn = QPushButton("Refresh GPIO & Outputs")
        self._refresh_gpio_btn.clicked.connect(self._on_refresh_gpio)
        g_btn_lay.addWidget(self._refresh_gpio_btn)
        g_btn_lay.addStretch()
        gpio_grid.addLayout(g_btn_lay, 11, 0, 1, 2)
        layout.addWidget(gpio_grp)

        # ── Chain / ISL probes ────────────────────────────────────────────────
        probe_grp = QGroupBox("Chain & ISL Probes")
        probe_lay = QVBoxLayout(probe_grp)
        p_btn_lay = QHBoxLayout()

        self._probe_cell_btn = QPushButton("Probe CELL Chain")
        self._probe_temp_btn = QPushButton("Probe TEMP Chain")
        self._probe_isl_btn  = QPushButton("Probe ISL28022")
        self._vpack_raw_btn  = QPushButton("Read Vpack Raw")
        for btn in (self._probe_cell_btn, self._probe_temp_btn,
                    self._probe_isl_btn, self._vpack_raw_btn):
            p_btn_lay.addWidget(btn)
        p_btn_lay.addStretch()

        self._probe_out = QTextEdit()
        self._probe_out.setReadOnly(True)
        self._probe_out.setMaximumHeight(120)
        self._probe_out.setFontFamily("Courier")
        probe_lay.addLayout(p_btn_lay)
        probe_lay.addWidget(self._probe_out)
        layout.addWidget(probe_grp)

        self._probe_cell_btn.clicked.connect(self._on_probe_cell)
        self._probe_temp_btn.clicked.connect(self._on_probe_temp)
        self._probe_isl_btn.clicked.connect( self._on_probe_isl)
        self._vpack_raw_btn.clicked.connect( self._on_vpack_raw)

        # ── One-shot measurements ─────────────────────────────────────────────
        meas_grp = QGroupBox("One-Shot Measurements")
        meas_lay = QVBoxLayout(meas_grp)
        m_btn_lay = QHBoxLayout()

        self._meas_cells_btn = QPushButton("Measure Cells Once")
        self._meas_temps_btn = QPushButton("Measure Temps Once")
        self._meas_power_btn = QPushButton("Measure Power Once")
        for btn in (self._meas_cells_btn, self._meas_temps_btn,
                    self._meas_power_btn):
            m_btn_lay.addWidget(btn)
        m_btn_lay.addStretch()

        self._meas_out = QTextEdit()
        self._meas_out.setReadOnly(True)
        self._meas_out.setMaximumHeight(100)
        self._meas_out.setFontFamily("Courier")
        meas_lay.addLayout(m_btn_lay)
        meas_lay.addWidget(self._meas_out)
        layout.addWidget(meas_grp)

        self._meas_cells_btn.clicked.connect(self._on_meas_cells)
        self._meas_temps_btn.clicked.connect(self._on_meas_temps)
        self._meas_power_btn.clicked.connect(self._on_meas_power)

        # ── Open-wire detection ───────────────────────────────────────────────
        ow_grp = QGroupBox("Open-Wire Detection (CELL Chain)")
        ow_lay = QVBoxLayout(ow_grp)
        ow_btn_lay = QHBoxLayout()

        self._run_ow_btn = QPushButton("Run Open-Wire")
        ow_btn_lay.addWidget(self._run_ow_btn)
        ow_btn_lay.addStretch()

        self._ow_out = QTextEdit()
        self._ow_out.setReadOnly(True)
        self._ow_out.setMaximumHeight(80)
        self._ow_out.setFontFamily("Courier")
        ow_lay.addLayout(ow_btn_lay)
        ow_lay.addWidget(self._ow_out)
        layout.addWidget(ow_grp)

        self._run_ow_btn.clicked.connect(self._on_run_openwire)

        # ── Safety actions ────────────────────────────────────────────────────
        safety_grp = QGroupBox("Safety Actions")
        safety_lay = QHBoxLayout(safety_grp)

        self._bal_disable_btn   = QPushButton("Balance Disable-All")
        self._clear_latched_btn = QPushButton("Clear Latched Faults")
        self._bal_disable_btn.setStyleSheet(
            "background-color:#b87000; color:white; font-weight:bold;")
        self._clear_latched_btn.setStyleSheet(
            "background-color:#8a0000; color:white; font-weight:bold;")
        safety_lay.addWidget(self._bal_disable_btn)
        safety_lay.addWidget(self._clear_latched_btn)
        safety_lay.addStretch()
        layout.addWidget(safety_grp)

        self._bal_disable_btn.clicked.connect(  self._on_balance_disable_all)
        self._clear_latched_btn.clicked.connect(self._on_clear_latched)

        layout.addStretch()

        self._all_action_btns = [
            self._refresh_diag_btn, self._refresh_gpio_btn,
            self._probe_cell_btn, self._probe_temp_btn,
            self._probe_isl_btn,  self._vpack_raw_btn,
            self._meas_cells_btn, self._meas_temps_btn, self._meas_power_btn,
            self._run_ow_btn,
            self._bal_disable_btn, self._clear_latched_btn,
        ]
        for btn in self._all_action_btns:
            btn.setEnabled(False)

    # ── Model helpers ─────────────────────────────────────────────────────────

    def _model(self):
        return getattr(self._main, '_model', None)

    def _emit(self, text_widget: QTextEdit, msg: str) -> None:
        text_widget.append(msg)

    def _run(self, fn, out: QTextEdit) -> None:
        model = self._model()
        if model is None:
            self._emit(out, "Not connected.")
            return
        try:
            result = fn(model)
            if isinstance(result, str):
                self._emit(out, result)
            else:
                import json
                self._emit(out, json.dumps(result, default=str))
        except TargetRefusedError as e:
            self._emit(out, f"Refused: {e}")
        except ProtocolError as e:
            self._emit(out, f"Protocol error: {e}")
        except Exception as e:
            self._emit(out, f"Error: {e}")

    # ── Slots ─────────────────────────────────────────────────────────────────

    def _on_refresh_diagnostics(self) -> None:
        model = self._model()
        if model is None:
            return
        try:
            d = model.poll_diagnostics()
            self._reset_cause.setText(f"0x{d.reset_cause:02X}")
            self._pec_cell_err.setText(str(d.pec_cell_errors))
            self._pec_temp_err.setText(str(d.pec_temp_errors))
            self._i2c_err.setText(str(d.i2c_errors))
            self._openwire_valid.setText("Yes" if d.open_wire_valid else "No")
            n_det = sum(
                1 for i in range(75)
                if d.open_wire_mask[i // 8] & (1 << (i % 8)))
            self._openwire_mask.setText(
                f"0x{d.open_wire_mask.hex()}  ({n_det} detected)")
            self._diag_uptime.setText(f"{d.uptime_ms / 1000:.1f}")
        except (TargetRefusedError, ProtocolError) as e:
            self._reset_cause.setText(f"Error: {e}")

    def _on_refresh_gpio(self) -> None:
        model = self._model()
        if model is None:
            return
        try:
            g = model.get_gpio_snapshot()
            o = model.get_outputs_snapshot()
            self._cs_cell.setText(str(g['cs_cell']))
            self._cs_temp.setText(str(g['cs_temp']))
            self._power_btn_lbl.setText(str(g['power_button']))
            self._charge_det.setText(str(g['charge_detect']))
            self._power_en.setText(str(g['power_enable']))
            self._master_ok.setText(str(g['master_ok_raw']))
            self._discharge_r.setText(str(g['discharge_raw']))
            self._charge_r.setText(str(g['charge_raw']))
            self._charger_safe.setText(str(g['charger_safety_raw']))
            self._out_logical.setText(f"0x{o['logical_state']:02X}")
            self._out_raw.setText(f"0x{o['raw_state']:02X}")
        except (TargetRefusedError, ProtocolError) as e:
            self._cs_cell.setText(f"Error: {e}")

    def _on_probe_cell(self) -> None:
        self._probe_out.clear()
        self._run(lambda m: m.probe_cell_chain(), self._probe_out)

    def _on_probe_temp(self) -> None:
        self._probe_out.clear()
        self._run(lambda m: m.probe_temp_chain(), self._probe_out)

    def _on_probe_isl(self) -> None:
        self._probe_out.clear()
        self._run(lambda m: m.probe_isl28022(), self._probe_out)

    def _on_vpack_raw(self) -> None:
        self._probe_out.clear()
        self._run(lambda m: m.read_vpack_raw(), self._probe_out)

    def _on_meas_cells(self) -> None:
        def _do(m):
            r = m.measure_cells_once()
            n_valid = sum(1 for v in r.get('validity', []) if v)
            return (f"status={r['status']}  cells={r['cell_count']}  "
                    f"valid={n_valid}  ts={r['timestamp_ms']} ms")
        self._meas_out.clear()
        self._run(_do, self._meas_out)

    def _on_meas_temps(self) -> None:
        def _do(m):
            r = m.measure_temps_once()
            valid = [t for t in r.get('temps_cx10', []) if t != -0x8000]
            return (f"status={r['status']}  temps={r['temp_count']}  "
                    f"valid={len(valid)}  ts={r['timestamp_ms']} ms")
        self._meas_out.clear()
        self._run(_do, self._meas_out)

    def _on_meas_power(self) -> None:
        def _do(m):
            r = m.measure_power_once()
            return (f"status={r['status']}  "
                    f"vbat={r.get('vbat_mv', '?')} mV  "
                    f"vpack={r.get('vpack_mv', '?')} mV  "
                    f"ibat={r.get('i_batt_ma', '?')} mA")
        self._meas_out.clear()
        self._run(_do, self._meas_out)

    def _on_run_openwire(self) -> None:
        def _do(m):
            r = m.run_openwire()
            detected = [
                i for i in range(75)
                if r['open_wire_mask'][i // 8] & (1 << (i % 8))
            ]
            lines = [f"status={r['status']}  detected={len(detected)} cells"]
            lines.append("  open cells: " + str(detected) if detected else "  all wires OK")
            return '\n'.join(lines)
        self._ow_out.clear()
        self._run(_do, self._ow_out)

    def _on_balance_disable_all(self) -> None:
        def _do(m):
            ok = m.balance_disable_all()
            return f"Balance Disable-All: {'OK' if ok else 'FAILED'}"
        self._run(_do, self._probe_out)

    def _on_clear_latched(self) -> None:
        model = self._model()
        if model is None:
            return
        try:
            cleared = model.clear_latched_faults(0xFFFFFFFFFFFFFFFF)
            evt_log = getattr(self._main, '_evt_log', None)
            if evt_log:
                evt_log.append(f"[Bringup] Cleared latched faults: 0x{cleared:016X}")
        except (TargetRefusedError, ProtocolError) as e:
            evt_log = getattr(self._main, '_evt_log', None)
            if evt_log:
                evt_log.append(f"[Bringup] clear_latched error: {e}")

    # ── State refresh ─────────────────────────────────────────────────────────

    def refresh(self, state: AppState) -> None:
        from ...connection.device_state import DeviceMode
        mode   = state.device.mode
        is_app = (mode == DeviceMode.BMS_APP)

        for btn in self._all_action_btns:
            btn.setEnabled(is_app)

        if not is_app:
            notice_text = "Bring-up actions require BMS_APP mode."
            if mode == DeviceMode.BOOTLOADER:
                notice_text = (
                    "Bring-up actions require BMS_APP mode.  "
                    "Current mode: BOOTLOADER.")
            elif mode == DeviceMode.DISCONNECTED:
                notice_text = "Bring-up actions require BMS_APP mode.  Not connected."
            self._mode_notice.setText(notice_text)
            self._mode_notice.setVisible(True)
        else:
            self._mode_notice.setVisible(False)

        d = state.diagnostics
        if is_app and d.valid:
            self._reset_cause.setText(f"0x{d.reset_cause:02X}")
            self._pec_cell_err.setText(str(d.pec_cell_errors))
            self._pec_temp_err.setText(str(d.pec_temp_errors))
            self._i2c_err.setText(str(d.i2c_errors))
            self._openwire_valid.setText("Yes" if d.open_wire_valid else "No")
            n_det = sum(
                1 for i in range(75)
                if d.open_wire_mask[i // 8] & (1 << (i % 8)))
            self._openwire_mask.setText(
                f"0x{d.open_wire_mask.hex()}  ({n_det} detected)")
            self._diag_uptime.setText(f"{d.uptime_ms / 1000:.1f}")
        elif not is_app:
            for lbl in (self._reset_cause, self._pec_cell_err,
                        self._pec_temp_err, self._i2c_err,
                        self._openwire_valid, self._openwire_mask,
                        self._diag_uptime, self._cs_cell, self._cs_temp,
                        self._power_btn_lbl, self._charge_det,
                        self._power_en, self._master_ok,
                        self._discharge_r, self._charge_r,
                        self._charger_safe, self._out_logical, self._out_raw):
                lbl.setText("—")
