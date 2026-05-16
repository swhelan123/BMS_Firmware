"""test_gui_smoke.py — GUI smoke tests (skipped if PyQt6 not installed).

Tests that GUI modules can be imported and basic models work without a display.
"""
import pytest

PyQt6 = pytest.importorskip('PyQt6', reason="PyQt6 not installed — GUI tests skipped")

# ── Backend models (no display needed) ───────────────────────────────────────

def test_app_state_import():
    from tool.src.core.app_state import AppState
    state = AppState()
    assert state is not None


def test_gui_main_importable():
    # Just importing — no app.exec() call
    import tool.src.gui.main as gui_main
    assert hasattr(gui_main, 'BmsMainWindow')


def test_gui_pages_importable():
    from tool.src.gui.pages import connection, dashboard, cells
    from tool.src.gui.pages import temperatures, faults, config
    from tool.src.gui.pages import firmware_flash, logs


def test_bms_main_window_constructs(qapp):
    from tool.src.gui.main import BmsMainWindow
    win = BmsMainWindow()
    assert win is not None
    win.close()
