"""test_gui_pages.py — GUI page construction and state propagation tests.

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


# ── Bringup page ──────────────────────────────────────────────────────────────

class TestBringupPage:
    def _make_page(self, qapp):
        from tool.src.gui.pages.bringup import BringupPage

        class _FakeMain:
            _model = None
        return BringupPage(AppState(), _FakeMain())

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
        assert page._vbat.text()  == "48000"
        assert page._vpack.text() == "47500"
        assert page._state.text() == "DISCHARGE"

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
        item = page._table.item(0, 0)  # cell 0 → row 0, col 0
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
        temps[0] = -0x8000  # INVALID
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
        from tool.src.gui.pages.faults import FaultsPage, _FAULT_NAMES
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


# ── Config page ───────────────────────────────────────────────────────────────

class TestConfigPage:
    def test_constructs(self, qapp):
        from tool.src.gui.pages.config import ConfigPage

        class _FakeMain:
            _model = None
        page = ConfigPage(AppState(), _FakeMain())
        assert page is not None

    def test_has_export_default_button(self, qapp):
        from tool.src.gui.pages.config import ConfigPage

        class _FakeMain:
            _model = None
        page = ConfigPage(AppState(), _FakeMain())
        assert hasattr(page, '_export_btn')


# ── Firmware Flash page ───────────────────────────────────────────────────────

class TestFirmwareFlashPage:
    def test_constructs(self, qapp):
        from tool.src.gui.pages.firmware_flash import FirmwareFlashPage

        class _FakeMain:
            _model = None
        page = FirmwareFlashPage(AppState(), _FakeMain())
        assert page is not None

    def test_simulation_section_present(self, qapp):
        from tool.src.gui.pages.firmware_flash import FirmwareFlashPage

        class _FakeMain:
            _model = None
        page = FirmwareFlashPage(AppState(), _FakeMain())
        assert hasattr(page, '_enter_bl_btn')
        assert hasattr(page, '_run_sim_btn')
        assert hasattr(page, '_abort_sim_btn')
        assert hasattr(page, '_sim_progress')

    def test_enter_bl_disabled_when_disconnected(self, qapp):
        from tool.src.gui.pages.firmware_flash import FirmwareFlashPage

        class _FakeMain:
            _model = None
        page = FirmwareFlashPage(AppState(), _FakeMain())
        page.refresh(AppState())
        assert not page._enter_bl_btn.isEnabled()

    def test_enter_bl_enabled_in_app_mode(self, qapp):
        from tool.src.gui.pages.firmware_flash import FirmwareFlashPage

        class _FakeMain:
            _model = None
        state = _app_state()
        page  = FirmwareFlashPage(state, _FakeMain())
        page.refresh(state)
        assert page._enter_bl_btn.isEnabled()


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
        assert win._tabs.count() == 9  # Connection + Bring-Up + 6 + Logs
        win.close()

    def test_runtime_tabs_disabled_when_disconnected(self, qapp):
        from tool.src.gui.main import BmsMainWindow
        win = BmsMainWindow()
        # Bring-Up = idx 1; Dashboard = idx 2; should be disabled at start
        assert not win._tabs.isTabEnabled(1)  # Bring-Up
        assert not win._tabs.isTabEnabled(2)  # Dashboard
        win.close()

    def test_connection_and_logs_always_enabled(self, qapp):
        from tool.src.gui.main import BmsMainWindow
        win = BmsMainWindow()
        assert win._tabs.isTabEnabled(0)  # Connection
        assert win._tabs.isTabEnabled(8)  # Logs
        win.close()
