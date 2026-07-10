"""charging.py — charge session monitor: charger CAN link, readiness, termination."""
from PyQt6.QtWidgets import (
    QWidget, QVBoxLayout, QHBoxLayout, QGridLayout,
    QGroupBox, QLabel, QPushButton,
)
from PyQt6.QtCore import Qt, pyqtSignal

from ...core.app_state import AppState
from ...protocol.bms_defs import (
    STATE_NAMES as _BMS_STATES, BMS_STATE_CHARGE, BMS_STATE_FAULT,
    FAULT_BLOCKS_CHARGE_MASK, fault_names_from_mask,
    OUTPUTS_BIT_CHARGE, OUTPUTS_BIT_CHARGER_SAFETY,
)
from .dashboard import _card, _CHARGER_FLAG_NAMES

_STYLE_VAL     = "font-size:16px; font-weight:bold;"
_STYLE_INVALID = "font-size:16px; font-weight:bold; color:#888888;"
_STYLE_GOOD    = "font-size:16px; font-weight:bold; color:#27ae60;"
_STYLE_WARN    = "font-size:16px; font-weight:bold; color:#d4a017;"
_STYLE_BAD     = "font-size:16px; font-weight:bold; color:#c0392b;"

# Charger status frames arrive at 1 Hz; anything much older means the CAN
# link is down or the charger is unpowered.
_STATUS_STALE_MS = 3000


class ChargingPage(QWidget):
    refresh_requested = pyqtSignal()

    def __init__(self, state: AppState, parent=None):
        super().__init__(parent)
        self._build_ui()

    def _build_ui(self) -> None:
        layout = QVBoxLayout(self)
        layout.setContentsMargins(10, 10, 10, 10)
        layout.setSpacing(8)

        # ── Actions ───────────────────────────────────────────────────────────
        act_grp = QGroupBox("Actions")
        act_lay = QHBoxLayout(act_grp)
        act_lay.setContentsMargins(8, 4, 8, 4)
        self._refresh_btn = QPushButton("Refresh Now")
        self._refresh_btn.clicked.connect(self.refresh_requested)
        act_lay.addWidget(self._refresh_btn)
        act_lay.addStretch()
        layout.addWidget(act_grp)

        # ── Session state ─────────────────────────────────────────────────────
        sess_grp  = QGroupBox("Charge Session")
        sess_grid = QGridLayout(sess_grp)
        sess_grid.setColumnMinimumWidth(0, 130)
        self._bms_state   = _card("BMS State:",       0, sess_grid)
        self._charge_perm = _card("Charge Perm:",     1, sess_grid)
        self._safety_out  = _card("Charger Safety:",  2, sess_grid)
        self._pack_i      = _card("Pack Current:",    3, sess_grid)
        self._max_cell    = _card("Highest Cell:",    4, sess_grid)

        # ── Charger CAN link ──────────────────────────────────────────────────
        link_grp  = QGroupBox("Charger Link (Elcon CAN)")
        link_grid = QGridLayout(link_grp)
        link_grid.setColumnMinimumWidth(0, 130)
        self._chg_voltage = _card("Output Voltage:", 0, link_grid)
        self._chg_current = _card("Output Current:", 1, link_grid)
        self._chg_flags   = _card("Charger Flags:",  2, link_grid)
        self._chg_age     = _card("Last Status:",    3, link_grid)
        self._chg_term    = _card("Terminating:",    4, link_grid)

        row = QHBoxLayout()
        row.addWidget(sess_grp)
        row.addWidget(link_grp)
        layout.addLayout(row)

        # ── Readiness ─────────────────────────────────────────────────────────
        ready_grp = QGroupBox("Charge Readiness")
        ready_lay = QVBoxLayout(ready_grp)
        self._ready_lbl = QLabel("—")
        self._ready_lbl.setStyleSheet(_STYLE_VAL)
        self._blockers_lbl = QLabel("")
        self._blockers_lbl.setWordWrap(True)
        self._blockers_lbl.setStyleSheet("font-size:13px; color:#c0392b;")
        ready_lay.addWidget(self._ready_lbl)
        ready_lay.addWidget(self._blockers_lbl)
        layout.addWidget(ready_grp)

        note = QLabel(
            "The BMS enters CHARGE automatically when the charger is detected "
            "and no charge-blocking fault is active. Setpoints (target voltage, "
            "current, taper) come from the stored config — see the Config tab.")
        note.setWordWrap(True)
        note.setStyleSheet("font-size:12px; color:#999999;")
        layout.addWidget(note)

        layout.addStretch()

    def refresh(self, state: AppState) -> None:
        vs = state.values
        cs = state.charger

        # ── Session ───────────────────────────────────────────────────────────
        if not vs.valid:
            for lbl in (self._bms_state, self._charge_perm, self._safety_out,
                        self._pack_i, self._max_cell, self._ready_lbl):
                lbl.setText("—")
                lbl.setStyleSheet(_STYLE_VAL)
            self._blockers_lbl.setText("")
        else:
            st_name = _BMS_STATES.get(vs.bms_state, str(vs.bms_state))
            self._bms_state.setText(st_name)
            if vs.bms_state == BMS_STATE_CHARGE:
                self._bms_state.setStyleSheet(_STYLE_GOOD)
            elif vs.bms_state == BMS_STATE_FAULT:
                self._bms_state.setStyleSheet(_STYLE_BAD)
            else:
                self._bms_state.setStyleSheet(_STYLE_VAL)

            chg_on  = bool(vs.outputs_state & OUTPUTS_BIT_CHARGE)
            safe_on = bool(vs.outputs_state & OUTPUTS_BIT_CHARGER_SAFETY)
            self._charge_perm.setText("asserted" if chg_on else "off")
            self._charge_perm.setStyleSheet(_STYLE_GOOD if chg_on else _STYLE_INVALID)
            self._safety_out.setText("asserted" if safe_on else "off")
            self._safety_out.setStyleSheet(_STYLE_GOOD if safe_on else _STYLE_INVALID)

            ibat_ok = bool(vs.measurement_flags & 0x04)
            self._pack_i.setText(
                f"{vs.i_batt_ma / 1000:.2f} A" if ibat_ok else "invalid")
            self._pack_i.setStyleSheet(_STYLE_VAL if ibat_ok else _STYLE_INVALID)

            # ── Readiness ─────────────────────────────────────────────────────
            blockers = vs.active_faults & FAULT_BLOCKS_CHARGE_MASK
            if vs.bms_state == BMS_STATE_CHARGE:
                self._ready_lbl.setText("CHARGING")
                self._ready_lbl.setStyleSheet(_STYLE_GOOD)
                self._blockers_lbl.setText("")
            elif blockers:
                names = fault_names_from_mask(blockers)
                self._ready_lbl.setText("BLOCKED — charge-inhibiting faults active")
                self._ready_lbl.setStyleSheet(_STYLE_BAD)
                self._blockers_lbl.setText("Blocking: " + ", ".join(names))
            else:
                self._ready_lbl.setText("READY — waiting for charger detect")
                self._ready_lbl.setStyleSheet(_STYLE_GOOD)
                self._blockers_lbl.setText("")

        # Highest cell — the termination trigger is any required cell
        # reaching cell_ov_soft_mv, so this is the number to watch.
        cells = state.cells
        if cells.valid and cells.cells_mv:
            n = cells.cell_count or len(cells.cells_mv)
            mv = cells.cells_mv[:n]
            if cells.validity:
                mv = [v for v, ok in zip(mv, cells.validity[:n]) if ok]
            if mv:
                self._max_cell.setText(f"{max(mv) / 1000:.3f} V")
                self._max_cell.setStyleSheet(_STYLE_VAL)
            else:
                self._max_cell.setText("no valid cells")
                self._max_cell.setStyleSheet(_STYLE_INVALID)
        else:
            self._max_cell.setText("—")
            self._max_cell.setStyleSheet(_STYLE_INVALID)

        # ── Charger link ──────────────────────────────────────────────────────
        if not cs.valid or not cs.status_valid:
            for lbl in (self._chg_voltage, self._chg_current, self._chg_flags,
                        self._chg_age):
                lbl.setText("no charger frames")
                lbl.setStyleSheet(_STYLE_INVALID)
            self._chg_term.setText("—")
            self._chg_term.setStyleSheet(_STYLE_INVALID)
            return

        stale = cs.status_age_ms >= _STATUS_STALE_MS
        self._chg_voltage.setText(f"{cs.output_voltage_dv / 10:.1f} V")
        self._chg_voltage.setStyleSheet(_STYLE_INVALID if stale else _STYLE_VAL)
        self._chg_current.setText(f"{cs.output_current_da / 10:.1f} A")
        self._chg_current.setStyleSheet(_STYLE_INVALID if stale else _STYLE_VAL)

        flags = [name for bit, name in _CHARGER_FLAG_NAMES.items()
                 if cs.status_flags & bit]
        self._chg_flags.setText(", ".join(flags) if flags else "OK")
        self._chg_flags.setStyleSheet(_STYLE_BAD if flags else _STYLE_GOOD)

        self._chg_age.setText(f"{cs.status_age_ms} ms ago"
                              + ("  (STALE)" if stale else ""))
        self._chg_age.setStyleSheet(_STYLE_INVALID if stale else _STYLE_VAL)

        self._chg_term.setText("yes" if cs.termination_requested else "no")
        self._chg_term.setStyleSheet(
            _STYLE_WARN if cs.termination_requested else _STYLE_VAL)
