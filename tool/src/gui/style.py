"""style.py — Shared style constants for BMS Tool GUI."""

# Mode badge: (background hex, foreground hex)
_MODE_PALETTE = {
    'BMS_APP':      ('#2a6b2a', '#ffffff'),
    'BOOTLOADER':   ('#9a6000', '#ffffff'),
    'UNSUPPORTED':  ('#8a0000', '#ffffff'),
    'DISCONNECTED': ('#555555', '#ffffff'),
}

MONOSPACE = "Courier New, Courier, monospace"

# Status-label colours
COLOR_OK      = '#1a6b1a'
COLOR_WARN    = '#9a6000'
COLOR_ERROR   = '#8a0000'
COLOR_NEUTRAL = '#444444'
COLOR_MODIFIED = '#0050a0'


def mode_badge_style(mode_name: str) -> str:
    bg, fg = _MODE_PALETTE.get(mode_name, ('#555555', '#ffffff'))
    return (
        f"background-color:{bg}; color:{fg}; "
        "font-weight:bold; padding:2px 10px; border-radius:3px;"
    )


def status_label_style(kind: str) -> str:
    """kind: 'ok' | 'warn' | 'error' | 'neutral' | 'modified'"""
    color = {
        'ok':      COLOR_OK,
        'warn':    COLOR_WARN,
        'error':   COLOR_ERROR,
        'neutral': COLOR_NEUTRAL,
        'modified': COLOR_MODIFIED,
    }.get(kind, COLOR_NEUTRAL)
    return f"color:{color}; font-weight:bold;"


# Shared stylesheet applied to the whole application
APP_STYLESHEET = """
QGroupBox {
    font-weight: bold;
    margin-top: 8px;
    padding-top: 4px;
}
QGroupBox::title {
    subcontrol-origin: margin;
    left: 8px;
}
QTableWidget {
    alternate-background-color: #f5f5f5;
}
QTextEdit[readOnly="true"] {
    background: #f8f8f8;
}
"""
