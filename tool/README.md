# BMS Desktop Tool

Monitoring, configuration, and firmware update tool for the BMS system.

## Install

```bash
pip install pyserial PyYAML pytest
pip install PyQt6   # GUI only
```

## Architecture

**Stack:** Python 3.11+ / PyQt6 / pyserial  
**Pattern:** Shared backend → thin CLI + PyQt6 GUI; backend owns all protocol/config/package logic

```
tool/
  src/
    core/
      app_state.py           Central state (AppState + *State dataclasses); observer pattern
      connection_manager.py  TcpPort wrapper + serial.Serial; returns port-compat object
      target_model.py        High-level protocol calls with safety enforcement
      polling.py             Background thread → AppState
      logging_model.py       PacketLog + EventLog
    protocol/
      framing.py             Frame encode/decode
      crc.py                 CRC-16/CCITT-FALSE
      client.py              BmsProtocolClient — blocking request/response with retry
      packet_defs.py         Packet ID constants
    config/
      schema.py              BmsConfig struct serialise/deserialise
      validator.py           Client-side validation (mirrors firmware)
    update/
      package_parser.py      Firmware .pkg parse and validate
      package_builder.py     Build .pkg from .bin
      stlink.py              STM32_Programmer_CLI wrapper (dry-run + execute gate)
    fake_target/
      fake_target.py         Full BMS simulator: in-process or TCP server; 10 modes
    cli/
      bmsctl.py              Thin CLI wrapper
    gui/
      main.py                PyQt6 main window + PollingLoop integration
      pages/                 One file per tab
    connection/
      device_state.py        DeviceMode, CapabilitiesState, DeviceState
  tests/
    test_framing.py
    test_crc.py
    test_config_validator.py
    test_fake_target.py
    test_package_parser.py
    test_diagnostics_and_modes.py
    test_backend.py          Core backend against FakeTargetInProcess
    test_cli.py              CLI commands via TCP loopback
    test_gui_smoke.py        GUI import/construct (skipped if PyQt6 absent)
```

## CLI

```bash
# Start fake target
python -m tool.src.cli.bmsctl fake-target run --mode healthy
python -m tool.src.cli.bmsctl fake-target self-test

# Runtime
python -m tool.src.cli.bmsctl connect
python -m tool.src.cli.bmsctl values [--json]
python -m tool.src.cli.bmsctl cells [-v] [--json]
python -m tool.src.cli.bmsctl temps [-v]
python -m tool.src.cli.bmsctl faults [--json]
python -m tool.src.cli.bmsctl diagnostics

# Config
python -m tool.src.cli.bmsctl config export-default --out default.bin
python -m tool.src.cli.bmsctl config validate default.bin
python -m tool.src.cli.bmsctl config read --out read.bin
python -m tool.src.cli.bmsctl config apply-ram default.bin
python -m tool.src.cli.bmsctl config diff a.bin b.bin

# Package
python -m tool.src.cli.bmsctl package build fw.bin fw.pkg --version 0.1.0
python -m tool.src.cli.bmsctl package inspect fw.pkg [--json]
python -m tool.src.cli.bmsctl package validate fw.pkg

# ST-Link (dry-run only — does not flash hardware)
python -m tool.src.cli.bmsctl stlink dry-run-app fw.pkg
```

## GUI

```bash
pip install PyQt6
python -m tool.src.gui.main --fake --mode healthy   # auto-connect to fake target
python -m tool.src.gui.main                         # connect via UI
```

Tabs: Connection | Dashboard | Cells | Temperatures | Faults | Config | Firmware Flash | Logs

Safety enforced in GUI:
- Runtime tabs disabled until `BMS_APP` mode confirmed
- Config/update buttons gated on hw_profile match
- Flash execute requires explicit safety checkbox
- No output-force or balancing-force buttons anywhere

## Fake Target Modes

| Mode | Description |
|------|-------------|
| `healthy` | Nominal 3700 mV cells, 25°C temps, no faults |
| `safe_invalid` | Zero cells, all temps INVALID, no faults |
| `cell_uv` | cell[0]=2400 mV, FAULT_CELL_UV active |
| `cell_ov` | cell[0]=4300 mV, FAULT_CELL_OV active |
| `temp_invalid` | All temps INVALID, FAULT_TEMP_READ_INVALID active |
| `vpack_invalid` | FAULT_VPACK_INVALID active |
| `isospi_fault` | FAULT_ISOSPI_CELL active |
| `config_error` | FAULT_CONFIG_INVALID active |
| `precharge_fault` | FAULT_PRECHARGE_TIMEOUT active |
| `bootloader` | capabilities returns FIRMWARE_TYPE_BOOTLOADER |

## Tests

```bash
python3 -m pytest tool/tests/ -q
# 144+ passed, 1 skipped (GUI test skipped without PyQt6)
```

## Safety / Refusal Rules

| Condition | Effect |
|-----------|--------|
| No capabilities handshake | all ops refused |
| hw_profile_id mismatch | UNSUPPORTED mode; config/update refused |
| bootloader mode | runtime ops refused |
| UNSUPPORTED mode | all config and update ops refused |
| ST-Link execute | requires `confirm=True` / explicit GUI checkbox |
| Package > 188 KB | PackageBuildError |
| Package empty | PackageBuildError |

---

## UI Architecture

- **Single QMainWindow** with a `QStackedWidget` for pages
- **Left sidebar navigation** (icons + labels) — disabled pages grayed out based on `DeviceMode`
- **Connection status bar** at top: port name, mode badge, firmware version
- Each page is a `QWidget` subclass; communicates with the backend via Qt signals/slots
- **Data refresh:** background `QThread` polls `GET_VALUES` and `GET_CELLS`/`GET_TEMPS` at configured rate; emits signals to update UI
- **Non-blocking serial I/O:** all serial operations run in a worker thread; UI updates via `pyqtSignal`

---

## Protocol Client Structure

```python
class BmsProtocolClient:
    def __init__(self, port: serial.Serial)
    def send_request(self, pkt_id: int, payload: bytes) -> bytes  # blocking, with timeout/retry
    def get_capabilities(self) -> CapabilitiesResponse
    def get_values(self) -> ValuesResponse
    def get_cells(self, include_validity: bool) -> GetCellsResponse
    def get_temps(self) -> GetTempsResponse
    def get_faults(self) -> GetFaultsResponse
    def clear_latched_faults(self, mask: int) -> int
    def get_config(self) -> BmsConfig
    def validate_config(self, cfg: BmsConfig) -> ConfigValidationResponse
    def set_config_ram(self, cfg: BmsConfig) -> ConfigValidationResponse
    def store_config(self, cfg: BmsConfig) -> StoreConfigResponse
    def enter_bootloader(self) -> None
    def get_boot_info(self) -> BootInfoResponse
    def boot_update_begin(self, header: FirmwarePackageHeader) -> BootUpdateBeginResponse
    def boot_update_chunk(self, index: int, data: bytes) -> BootUpdateChunkResponse
    def boot_update_finalize(self) -> BootUpdateFinalizeResponse
    def boot_update_abort(self) -> None
```

All methods raise `ProtocolError` subclasses on timeout, bad CRC, or error response.

---

## Fake Target Strategy

`fake_target.py` runs as either:
1. A subprocess listening on a virtual serial port pair (for tool integration tests)
2. An in-process mock (for unit tests of protocol client)

Scenario files (`scenarios/*.yaml`) define:
- Cell voltages (array of 75 values or a pattern)
- Temperature values (array of 75 values)
- Active faults bitmask
- Latched faults bitmask
- State
- Whether to inject PEC errors

The fake target is the primary CI test vehicle. All tool flows are tested against it in GitHub Actions before any hardware is available.

---

## Config Editor Strategy

`config/editor_widget.py` is **generated** from `protocol/config_schema.yaml` by `scripts/gen_config_schema.py`. The generator produces:
- A `QFormLayout`-based editor with one row per config field
- Type-appropriate input widgets (QSpinBox, QDoubleSpinBox, QLineEdit for masks)
- Range hints shown as placeholder/tooltip text
- Red highlight on out-of-range values
- Threshold ordering violations shown inline

To add a config field: edit `protocol/config_schema.yaml` → run `gen_config_schema.py` → rebuild tool.

---

## Firmware Flashing Strategy

**ST-Link path (development):**
```python
class StLinkFlasher:
    def detect(self) -> list[StLinkInfo]  # calls STM32_Programmer_CLI --list
    def flash(self, path: str, address: int, on_progress: Callable) -> FlashResult
    # Runs: STM32_Programmer_CLI -c port=SWD -d <path> <address> -v -rst
```

**Bootloader path (production/service):**
```python
class BootloaderUpdater:
    def __init__(self, client: BmsProtocolClient)
    def update(self, pkg_path: str, on_progress: Callable[[int, int], None]) -> UpdateResult
    # Validates package, enters bootloader, chunks, finalizes, reconnects
```

Both operate on real `BmsProtocolClient` or `StLinkFlasher` — the UI just calls these and displays progress.

---

## Packaging

PyInstaller one-folder bundle:
```bash
pyinstaller bms_tool.spec
```

Output: `dist/BmsTool/` (folder) or `dist/BmsTool` (single exe with `--onefile`).

CI builds produce signed packages for macOS and unsigned installer for Windows. Linux users can run from source.

Version baked in from `git describe --tags`.

---

## Test Approach

1. `pytest tests/test_framing.py` — unit test packet encode/decode, CRC, error injection
2. `pytest tests/test_config_validator.py` — client-side validation matches firmware validation
3. `pytest tests/test_fake_target.py` — full protocol flows (requires `fake_target.py` subprocess)
4. `pytest tests/test_package_parser.py` — package header parse, bad-header detection

All tests run in CI without hardware. Hardware-in-loop tests are manual (see `docs/08_validation_plan.md`).
