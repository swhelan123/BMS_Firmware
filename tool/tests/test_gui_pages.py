"""test_gui_pages.py — GUI page construction and state-propagation tests.

All tests run headless with a qapp fixture (provided by pytest-qt or a conftest).
Skipped automatically when PyQt6 is not installed.
"""
import pytest

PyQt6 = pytest.importorskip('PyQt6', reason="PyQt6 not installed — GUI tests skipped")

from tool.src.core.app_state import (
    AppState, ValuesState, CellsState, TempsState, FaultsState,
    DiagnosticsState,
)
from tool.src.connection.device_state import DeviceState, DeviceMode, CapabilitiesState


# ── helpers ───────────────────────────────────────────────────────────────────

def _make_caps():
    return CapabilitiesState(
        firmware_type=1, firmware_version=(1, 2, 3),
        hw_profile_id=0xB1A5, protocol_version=1,
        config_schema_version=1, cell_count=75, temp_count=75,
        feature_flags=0,
    )

def _app_state() -> AppState:
    s = AppState()
    s.update_device(DeviceState(mode=DeviceMode.BMS_APP, capabilities=_make_caps()))
    return s

def _bl_state() -> AppState:
    s = AppState()
    s.update_device(DeviceState(mode=DeviceMode.BOOTLOADER, capabilities=_make_caps()))
    return s

def _disconnected_state() -> AppState:
    return AppState()


# ── Connection page ───────────────────────────────────────────────────────────

class TestConnectionPage:
    def test_constructs(self, qapp):
        from tool.src.gui.pages.connection import ConnectionPage
        page = ConnectionPage(AppState())
        assert page is not None

    def test_has_serial_section(self, qapp):
        from tool.src.gui.pages.connection import ConnectionPage
        page = ConnectionPage(AppState())
        assert hasattr(page, '_serial_combo')
        assert hasattr(page, '_baud_spin')
        assert hasattr(page, '_connect_ser_btn')

    def test_connect_buttons_enabled_when_disconnected(self, qapp):
        from tool.src.gui.pages.connection import ConnectionPage
        page = ConnectionPage(_disconnected_state())
        assert page._connect_tcp_btn.isEnabled()
        assert page._connect_ser_btn.isEnabled()
        assert not page._disconnect_btn.isEnabled()

    def test_disconnect_button_enabled_when_connected(self, qapp):
        from tool.src.gui.pages.connection import ConnectionPage
        state = _app_state()
        page  = ConnectionPage(state)
        page.refresh(state)
        assert not page._connect_tcp_btn.isEnabled()
        assert page._disconnect_btn.isEnabled()

    def test_tcp_signal_emitted(self, qapp):
        from tool.src.gui.pages.connection import ConnectionPage
        page = ConnectionPage(AppState())
        received = []
        page.connect_requested.connect(lambda h, p: received.append((h, p)))
        page._on_connect_tcp()
        assert received == [('127.0.0.1', 65102)]

    def test_serial_signal_emitted(self, qapp):
        from tool.src.gui.pages.connection import ConnectionPage
        page = ConnectionPage(AppState())
        received = []
        page.connect_serial_requested.connect(lambda d, b: received.append((d, b)))
        page._serial_combo.setCurrentText('/dev/ttyUSB0')
        page._on_connect_serial()
        assert len(received) == 1
        assert received[0][0] == '/dev/ttyUSB0'
        assert received[0][1] == 115200

    def test_hints_contain_bmsctl_script(self, qapp):
        from tool.src.gui.pages.connection import ConnectionPage
        page = ConnectionPage(AppState())
        # Find all QLabel texts in the hint group
        from PyQt6.QtWidgets import QLabel
        labels = page.findChildren(QLabel)
        texts = [lbl.text() for lbl in labels]
        # Must contain at least one reference to the correct script wrapper
        assert any("bmsctl.sh" in t or "run_fake_hardware.sh" in t
                   or "run_gui.sh" in t for t in texts)
        # Must NOT contain the old wrong invocation
        assert not any(
            "fake_target.fake_target" in t and "--mode" in t
            for t in texts
        )


# ── Bringup page ──────────────────────────────────────────────────────────────

class TestBringupPage:
    def _make_page(self, qapp, state=None):
        from tool.src.gui.pages.bringup import BringupPage

        class _FakeMain:
            _model = None
        return BringupPage(state or AppState(), _FakeMain())

    def test_constructs(self, qapp):
        page = self._make_page(qapp)
        assert page is not None

    def test_all_buttons_disabled_when_disconnected(self, qapp):
        page = self._make_page(qapp)
        for btn in page._all_action_btns:
            assert not btn.isEnabled(), f"{btn.text()} should be disabled when disconnected"

    def test_buttons_enabled_after_app_connect(self, qapp):
        from tool.src.gui.pages.bringup import BringupPage

        class _FakeMain:
            _model = None
        state = _app_state()
        page  = BringupPage(state, _FakeMain())
        page.refresh(state)
        for btn in page._all_action_btns:
            assert btn.isEnabled(), f"{btn.text()} should be enabled in BMS_APP"

    def test_mode_notice_hidden_in_app_mode(self, qapp):
        from tool.src.gui.pages.bringup import BringupPage

        class _FakeMain:
            _model = None
        state = _app_state()
        page  = BringupPage(state, _FakeMain())
        page.refresh(state)
        assert not page._mode_notice.isVisible()

    def test_mode_notice_visible_in_bootloader_mode(self, qapp):
        from tool.src.gui.pages.bringup import BringupPage

        class _FakeMain:
            _model = None
        state = _bl_state()
        page  = BringupPage(state, _FakeMain())
        page.refresh(state)
        assert page._mode_notice.isVisible()
        assert "BOOTLOADER" in page._mode_notice.text()

    def test_mode_notice_visible_when_disconnected(self, qapp):
        from tool.src.gui.pages.bringup import BringupPage

        class _FakeMain:
            _model = None
        state = _disconnected_state()
        page  = BringupPage(state, _FakeMain())
        page.refresh(state)
        assert page._mode_notice.isVisible()

    def test_labels_reset_on_disconnect(self, qapp):
        from tool.src.gui.pages.bringup import BringupPage

        class _FakeMain:
            _model = None
        state = _disconnected_state()
        page  = BringupPage(state, _FakeMain())
        page.refresh(state)
        assert page._reset_cause.text() == "—"

    def test_diagnostics_state_auto_updates(self, qapp):
        from tool.src.gui.pages.bringup import BringupPage

        class _FakeMain:
            _model = None
        state = _app_state()
        state.update_diagnostics(DiagnosticsState(
            reset_cause=2, pec_cell_errors=3, pec_temp_errors=0,
            i2c_errors=1, open_wire_valid=True,
            open_wire_mask=bytes(10), uptime_ms=5000, valid=True,
        ))
        page = BringupPage(state, _FakeMain())
        page.refresh(state)
        assert page._reset_cause.text() == "0x02"
        assert page._pec_cell_err.text() == "3"
        assert page._openwire_valid.text() == "Yes"
        assert page._diag_uptime.text() == "5.0"


# ── Dashboard page ────────────────────────────────────────────────────────────

class TestDashboardPage:
    def test_constructs(self, qapp):
        from tool.src.gui.pages.dashboard import DashboardPage
        page = DashboardPage(AppState())
        assert page is not None

    def test_shows_dashes_when_no_data(self, qapp):
        from tool.src.gui.pages.dashboard import DashboardPage
        page = DashboardPage(AppState())
        page.refresh(AppState())
        assert page._vbat.text() == "—"
        assert page._vpack.text() == "—"

    def test_shows_values_when_valid(self, qapp):
        from tool.src.gui.pages.dashboard import DashboardPage
        state = AppState()
        state.update_values(ValuesState(
            vbat_mv=48000, vpack_mv=47500, i_batt_ma=-2000,
            bms_state=3, uptime_ms=10000, outputs_state=0x01,
            active_faults=0, latched_faults=0,
            measurement_flags=0, valid=True,
        ))
        page = DashboardPage(state)
        page.refresh(state)
        assert "48000" in page._vbat.text()
        assert "47500" in page._vpack.text()
        assert page._state.text() == "DISCHARGE"

    def test_invalid_values_show_dash(self, qapp):
        from tool.src.gui.pages.dashboard import DashboardPage
        page = DashboardPage(AppState())
        page.refresh(AppState())  # no valid ValuesState
        assert page._vbat.text() == "—"
        assert page._ibat.text() == "—"

    def test_shows_latched_count(self, qapp):
        from tool.src.gui.pages.dashboard import DashboardPage
        state = AppState()
        state.update_values(ValuesState(
            latched_faults=0b111, active_faults=0, valid=True,
        ))
        page = DashboardPage(state)
        page.refresh(state)
        assert "3 latched" in page._latched.text()

    def test_polling_toggle_signal(self, qapp):
        from tool.src.gui.pages.dashboard import DashboardPage
        page = DashboardPage(AppState())
        fired = []
        page.polling_toggle_requested.connect(lambda: fired.append(1))
        page._on_polling_toggle()
        assert len(fired) == 1

    def test_refresh_now_signal(self, qapp):
        from tool.src.gui.pages.dashboard import DashboardPage
        page = DashboardPage(AppState())
        fired = []
        page.refresh_now_requested.connect(lambda: fired.append(1))
        page._refresh_btn.click()
        assert len(fired) == 1


# ── Cells page ────────────────────────────────────────────────────────────────

class TestCellsPage:
    def test_constructs(self, qapp):
        from tool.src.gui.pages.cells import CellsPage
        page = CellsPage(AppState())
        assert page is not None

    def test_table_dimensions(self, qapp):
        from tool.src.gui.pages.cells import CellsPage
        page = CellsPage(AppState())
        assert page._table.rowCount() == 15
        assert page._table.columnCount() == 5

    def test_measure_once_signal(self, qapp):
        from tool.src.gui.pages.cells import CellsPage
        page = CellsPage(AppState())
        fired = []
        page.measure_once_requested.connect(lambda: fired.append(1))
        page._meas_btn.click()
        assert len(fired) == 1

    def test_refresh_signal(self, qapp):
        from tool.src.gui.pages.cells import CellsPage
        page = CellsPage(AppState())
        fired = []
        page.refresh_requested.connect(lambda: fired.append(1))
        page._refresh_btn.click()
        assert len(fired) == 1

    def test_invalid_cell_highlighted(self, qapp):
        from tool.src.gui.pages.cells import CellsPage
        from PyQt6.QtGui import QColor
        state = AppState()
        validity = [True] * 75
        validity[0] = False
        state.update_cells(CellsState(
            cell_count=75,
            cells_mv=[3700] * 75,
            validity=validity,
            timestamp_ms=1000,
            valid=True,
        ))
        page = CellsPage(state)
        page.refresh(state)
        item = page._table.item(0, 0)
        assert item is not None
        assert item.background().color() == QColor(255, 80, 80)

    def test_timestamp_shown(self, qapp):
        from tool.src.gui.pages.cells import CellsPage
        state = AppState()
        state.update_cells(CellsState(
            cell_count=75, cells_mv=[3700]*75,
            validity=None, timestamp_ms=42000, valid=True,
        ))
        page = CellsPage(state)
        page.refresh(state)
        assert "42000" in page._ts_lbl.text()


# ── Temperatures page ─────────────────────────────────────────────────────────

class TestTemperaturesPage:
    def test_constructs(self, qapp):
        from tool.src.gui.pages.temperatures import TemperaturesPage
        page = TemperaturesPage(AppState())
        assert page is not None

    def test_table_dimensions(self, qapp):
        from tool.src.gui.pages.temperatures import TemperaturesPage
        page = TemperaturesPage(AppState())
        assert page._table.rowCount() == 15
        assert page._table.columnCount() == 5

    def test_measure_once_signal(self, qapp):
        from tool.src.gui.pages.temperatures import TemperaturesPage
        page = TemperaturesPage(AppState())
        fired = []
        page.measure_once_requested.connect(lambda: fired.append(1))
        page._meas_btn.click()
        assert len(fired) == 1

    def test_invalid_sensor_highlighted(self, qapp):
        from tool.src.gui.pages.temperatures import TemperaturesPage
        from PyQt6.QtGui import QColor
        temps = [250] * 75
        temps[0] = -0x8000
        state = AppState()
        state.update_temps(TempsState(temp_count=75, temps_cx10=temps, valid=True))
        page = TemperaturesPage(state)
        page.refresh(state)
        item = page._table.item(0, 0)
        assert item is not None
        assert item.background().color() == QColor(255, 80, 80)
        assert "INVALID" in item.text()


# ── Faults page ───────────────────────────────────────────────────────────────

class TestFaultsPage:
    def test_constructs(self, qapp):
        from tool.src.gui.pages.faults import FaultsPage
        page = FaultsPage(AppState(), None)
        assert page is not None

    def test_refresh_signal(self, qapp):
        from tool.src.gui.pages.faults import FaultsPage
        page = FaultsPage(AppState(), None)
        fired = []
        page.refresh_requested.connect(lambda: fired.append(1))
        page._refresh_btn.click()
        assert len(fired) == 1

    def test_named_fault_in_table(self, qapp):
        from tool.src.gui.pages.faults import FaultsPage
        state = AppState()
        state.update_faults(FaultsState(
            active_faults=1,   # bit 0 = CELL_OV
            latched_faults=2,  # bit 1 = CELL_UV
            valid=True,
        ))
        page = FaultsPage(state, None)
        page.refresh(state)
        assert page._table.rowCount() == 2
        names = {page._table.item(r, 1).text() for r in range(2)}
        assert "CELL_OV" in names
        assert "CELL_UV" in names

    def test_clear_latched_enabled_when_faults(self, qapp):
        from tool.src.gui.pages.faults import FaultsPage
        state = AppState()
        state.update_faults(FaultsState(latched_faults=0xFF, valid=True))
        page = FaultsPage(state, None)
        page.refresh(state)
        assert page._clear_btn.isEnabled()

    def test_clear_latched_disabled_when_no_faults(self, qapp):
        from tool.src.gui.pages.faults import FaultsPage
        state = AppState()
        state.update_faults(FaultsState(active_faults=0, latched_faults=0, valid=True))
        page = FaultsPage(state, None)
        page.refresh(state)
        assert not page._clear_btn.isEnabled()

    def test_no_fault_message_shown_when_valid_and_clean(self, qapp):
        from tool.src.gui.pages.faults import FaultsPage
        state = AppState()
        state.update_faults(FaultsState(active_faults=0, latched_faults=0, valid=True))
        page = FaultsPage(state, None)
        page.refresh(state)
        assert page._state_lbl.isVisible()
        assert "no" in page._state_lbl.text().lower() or "fault" in page._state_lbl.text().lower()
        assert page._table.rowCount() == 0

    def test_unavailable_message_shown_when_invalid(self, qapp):
        from tool.src.gui.pages.faults import FaultsPage
        state = AppState()
        # FaultsState defaults to valid=False
        page = FaultsPage(state, None)
        page.refresh(state)
        assert page._state_lbl.isVisible()
        assert "unavailable" in page._state_lbl.text().lower()

    def test_state_label_hidden_when_faults_present(self, qapp):
        from tool.src.gui.pages.faults import FaultsPage
        state = AppState()
        state.update_faults(FaultsState(active_faults=1, latched_faults=0, valid=True))
        page = FaultsPage(state, None)
        page.refresh(state)
        assert not page._state_lbl.isVisible()
        assert page._table.isVisible()


# ── Config page ───────────────────────────────────────────────────────────────

class TestConfigPage:
    def _make_page(self, qapp, state=None):
        from tool.src.gui.pages.config import ConfigPage

        class _FakeMain:
            _model = None
        return ConfigPage(state or AppState(), _FakeMain())

    def test_constructs(self, qapp):
        page = self._make_page(qapp)
        assert page is not None

    def test_has_editor_and_raw_tabs(self, qapp):
        page = self._make_page(qapp)
        tab_titles = [page._tabs.tabText(i) for i in range(page._tabs.count())]
        assert "Editor" in tab_titles
        assert "Raw View" in tab_titles

    def test_has_action_buttons(self, qapp):
        page = self._make_page(qapp)
        assert hasattr(page, '_read_btn')
        assert hasattr(page, '_export_btn')
        assert hasattr(page, '_apply_btn')
        assert hasattr(page, '_val_btn')

    def test_no_store_to_flash_button(self, qapp):
        page = self._make_page(qapp)
        assert not hasattr(page, '_store_btn')

    def test_read_btn_disabled_when_disconnected(self, qapp):
        page = self._make_page(qapp)
        page.refresh(AppState())
        assert not page._read_btn.isEnabled()

    def test_read_btn_enabled_in_app_mode(self, qapp):
        state = _app_state()
        page  = self._make_page(qapp, state)
        page.refresh(state)
        assert page._read_btn.isEnabled()

    def test_apply_btn_disabled_without_config(self, qapp):
        state = _app_state()
        page  = self._make_page(qapp, state)
        page.refresh(state)
        # No config loaded yet
        assert not page._apply_btn.isEnabled()

    def test_loads_default_config_into_widgets(self, qapp):
        from tool.src.config.schema import BmsConfig
        page = self._make_page(qapp)
        cfg  = BmsConfig()
        page._cfg = cfg
        page._cfg_to_widgets(cfg)
        # Verify a representative field
        assert page._f['cell_uv_hard_mv'].value() == cfg.cell_uv_hard_mv
        assert page._f['cell_ov_hard_mv'].value() == cfg.cell_ov_hard_mv
        assert page._f['overcurrent_hard_ma'].value() == cfg.overcurrent_hard_ma

    def test_editing_spinbox_marks_dirty(self, qapp):
        from tool.src.config.schema import BmsConfig
        page = self._make_page(qapp)
        cfg  = BmsConfig()
        page._cfg = cfg
        page._cfg_to_widgets(cfg)
        assert not page._dirty
        # Change a value
        page._f['cell_uv_hard_mv'].setValue(page._f['cell_uv_hard_mv'].value() + 10)
        assert page._dirty

    def test_mask_fields_present(self, qapp):
        page = self._make_page(qapp)
        assert 'required_cell_mask' in page._f
        assert 'required_temp_mask' in page._f
        assert 'balance_allowed_mask' in page._f

    def test_default_config_passes_offline_validation(self, qapp):
        from tool.src.config.schema import BmsConfig
        from tool.src.config.validator import validate_config
        page = self._make_page(qapp)
        cfg  = BmsConfig()
        page._cfg = cfg
        page._cfg_to_widgets(cfg)
        # validate_masks should pass
        assert page._validate_masks() is None
        # Full validation via widgets_to_cfg
        built = page._widgets_to_cfg()
        ok, _, msg = validate_config(built)
        assert ok, f"Default config failed validation: {msg}"

    def test_invalid_mask_hex_detected(self, qapp):
        from tool.src.config.schema import BmsConfig
        page = self._make_page(qapp)
        cfg  = BmsConfig()
        page._cfg = cfg
        page._cfg_to_widgets(cfg)
        # Set invalid (too short) mask
        page._f['required_cell_mask'].setText("abcdef")
        err = page._validate_masks()
        assert err is not None
        assert "required_cell_mask" in err

    def test_raw_tab_updates_after_load(self, qapp):
        from tool.src.config.schema import BmsConfig
        page = self._make_page(qapp)
        cfg  = BmsConfig()
        page._cfg = cfg
        page._cfg_to_widgets(cfg)
        assert "cell_uv_hard_mv" in page._raw_text.toPlainText()

    def test_status_label_shows_modified_on_edit(self, qapp):
        from tool.src.config.schema import BmsConfig
        page = self._make_page(qapp)
        cfg  = BmsConfig()
        page._cfg = cfg
        page._cfg_to_widgets(cfg)
        page._f['cell_uv_hard_mv'].setValue(page._f['cell_uv_hard_mv'].value() + 10)
        assert "Modified" in page._status_lbl.text()

    def test_ro_header_labels_populated(self, qapp):
        from tool.src.config.schema import BmsConfig
        from tool.src.protocol.packet_defs import HW_PROFILE_ID
        page = self._make_page(qapp)
        cfg  = BmsConfig()
        page._cfg = cfg
        page._cfg_to_widgets(cfg)
        assert f"0x{HW_PROFILE_ID:04X}" in page._ro_hw_profile.text()
        assert str(cfg.cell_count) in page._ro_cell_count.text()


# ── Firmware Flash page ───────────────────────────────────────────────────────

class TestFirmwareFlashPage:
    def _make_page(self, qapp, state=None):
        from tool.src.gui.pages.firmware_flash import FirmwareFlashPage

        class _FakeMain:
            _model = None
            _state = state or AppState()
        return FirmwareFlashPage(state or AppState(), _FakeMain())

    def test_constructs(self, qapp):
        page = self._make_page(qapp)
        assert page is not None

    def test_simulation_section_present(self, qapp):
        page = self._make_page(qapp)
        assert hasattr(page, '_enter_bl_btn')
        assert hasattr(page, '_run_sim_btn')
        assert hasattr(page, '_abort_sim_btn')
        assert hasattr(page, '_sim_progress')

    def test_enter_bl_disabled_when_disconnected(self, qapp):
        page = self._make_page(qapp)
        page.refresh(AppState())
        assert not page._enter_bl_btn.isEnabled()

    def test_enter_bl_enabled_in_app_mode(self, qapp):
        state = _app_state()
        page  = self._make_page(qapp, state)
        page.refresh(state)
        assert page._enter_bl_btn.isEnabled()

    def test_run_sim_disabled_without_package(self, qapp):
        state = _bl_state()
        page  = self._make_page(qapp, state)
        page.refresh(state)
        # No package selected, so run_sim should be disabled
        assert not page._run_sim_btn.isEnabled()

    def test_bootloader_mode_banner_visible(self, qapp):
        state = _bl_state()
        page  = self._make_page(qapp, state)
        page.refresh(state)
        assert page._mode_lbl.isVisible()
        assert "BOOTLOADER" in page._mode_lbl.text()

    def test_bootloader_mode_banner_hidden_in_app_mode(self, qapp):
        state = _app_state()
        page  = self._make_page(qapp, state)
        page.refresh(state)
        assert not page._mode_lbl.isVisible()


# ── BmsMainWindow integration ─────────────────────────────────────────────────

class TestBmsMainWindow:
    def test_constructs(self, qapp):
        from tool.src.gui.main import BmsMainWindow
        win = BmsMainWindow()
        assert win is not None
        win.close()

    def test_has_bringup_tab(self, qapp):
        from tool.src.gui.main import BmsMainWindow
        win = BmsMainWindow()
        titles = [win._tabs.tabText(i) for i in range(win._tabs.count())]
        assert "Bring-Up" in titles
        win.close()

    def test_tab_count(self, qapp):
        from tool.src.gui.main import BmsMainWindow
        win = BmsMainWindow()
        assert win._tabs.count() == 9
        win.close()

    def test_runtime_tabs_disabled_when_disconnected(self, qapp):
        from tool.src.gui.main import BmsMainWindow
        win = BmsMainWindow()
        assert not win._tabs.isTabEnabled(1)  # Bring-Up
        assert not win._tabs.isTabEnabled(2)  # Dashboard
        win.close()

    def test_connection_and_logs_always_enabled(self, qapp):
        from tool.src.gui.main import BmsMainWindow
        win = BmsMainWindow()
        assert win._tabs.isTabEnabled(0)  # Connection
        assert win._tabs.isTabEnabled(8)  # Logs
        win.close()

    def test_polling_stops_on_bootloader_transition(self, qapp):
        from tool.src.gui.main import BmsMainWindow
        from tool.src.core.polling import PollingLoop
        win = BmsMainWindow()

        # Simulate a running polling loop
        class _MockPolling:
            stopped = False
            running = True
            def stop(self, timeout=2.0):
                self.stopped = True
            def start(self):
                pass

        mock_poll = _MockPolling()
        win._polling = mock_poll

        # Simulate device state changing to BOOTLOADER
        bl_state = _bl_state()
        win._state.update_device(bl_state.device)
        # _on_state_updated called synchronously via subscriber
        win._on_state_updated('device')

        assert mock_poll.stopped, "Polling should stop when device enters BOOTLOADER mode"
        assert win._polling is None
        win.close()

    def test_status_banner_shows_app_mode(self, qapp):
        from tool.src.gui.main import BmsMainWindow
        win = BmsMainWindow()
        win._update_banner(_app_state().device)
        assert "BMS_APP" in win._mode_badge.text()
        win.close()

    def test_status_banner_shows_bootloader_warning(self, qapp):
        from tool.src.gui.main import BmsMainWindow
        win = BmsMainWindow()
        win._update_banner(_bl_state().device)
        assert "BOOTLOADER" in win._mode_badge.text()
        assert win._banner_warn.isVisible()
        win.close()

    def test_status_banner_hides_warning_in_app_mode(self, qapp):
        from tool.src.gui.main import BmsMainWindow
        win = BmsMainWindow()
        win._update_banner(_app_state().device)
        assert not win._banner_warn.isVisible()
        win.close()
