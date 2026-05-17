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
      bootloader_updater.py  BootloaderUpdater: BEGIN→CHUNK*→FINALIZE over protocol
      stlink.py              STM32_Programmer_CLI wrapper (dry-run + execute gate)
    fake_target/
      fake_target.py         Static BMS simulator: in-process or TCP server; 12 modes
      live_simulator.py      Live simulator: evolving values over time; 10 modes
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
    test_live_simulator.py
    test_package_parser.py
    test_diagnostics_and_modes.py
    test_backend.py          Core backend against FakeTargetInProcess
    test_cli.py              CLI commands via TCP loopback
    test_bootloader_update.py Bootloader update protocol + BootloaderUpdater + CLI
    test_gui_smoke.py        GUI import/construct (skipped if PyQt6 absent)
    test_gui_pages.py        Per-page construction, signal, and state tests
```

## CLI

```bash
# Static fake target (one mode, stable values)
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
python -m tool.src.cli.bmsctl config export-json [file.bin] [--out file.json]
python -m tool.src.cli.bmsctl config import-json file.json [--out file.bin]
python -m tool.src.cli.bmsctl config export-yaml [file.bin] [--out file.yaml]

# Open-wire detection
python -m tool.src.cli.bmsctl openwire run [--json]

# Package
python -m tool.src.cli.bmsctl package build fw.bin fw.pkg --version 0.1.0
python -m tool.src.cli.bmsctl package inspect fw.pkg [--json]
python -m tool.src.cli.bmsctl package validate fw.pkg

# Bootloader update (simulation — same protocol used by real hardware)
python -m tool.src.cli.bmsctl update dry-run fw.pkg
python -m tool.src.cli.bmsctl update validate fw.pkg
python -m tool.src.cli.bmsctl update simulate fw.pkg

# ST-Link (dry-run only — does not flash hardware)
python -m tool.src.cli.bmsctl stlink dry-run-app fw.pkg
```

## GUI

Launch via the provided shell script (activates .venv automatically):

```bash
./scripts/run_gui.sh                          # open GUI, connect manually
./scripts/run_gui.sh --fake                   # auto-start static fake target + connect
./scripts/run_gui.sh --fake --mode cell_uv    # specific simulation mode
./scripts/run_gui.sh --fake --mode bootloader # test update flow
```

Tabs: Connection | Bring-Up | Dashboard | Cells | Temperatures | Faults | Config | Firmware Flash | Logs

**Connection page** — TCP (for fake target) and serial (for hardware) with baud-rate selector.
Quick-start CLI hints printed on the page.

**Bring-Up / Diagnostics page** — bench bring-up without hardware:
- Displays diagnostics counters (reset cause, PEC errors, I2C errors, open-wire mask, uptime)
- GPIO + outputs snapshot (CS_CELL/TEMP, power button, charge detect, master-ok, etc.)
- Chain probe buttons: Probe CELL Chain, Probe TEMP Chain, Probe ISL28022, Read Vpack Raw
- One-shot measurements: Measure Cells Once, Measure Temps Once, Measure Power Once
- Open-wire detection: Run Open-Wire, shows detected cell indices
- Safety actions: Balance Disable-All, Clear Latched Faults

**Dashboard** — polling start/stop + refresh-now controls; latched fault count; "—" on invalid data.

**Cells / Temperatures** — measure-once and refresh-snapshot buttons; snapshot timestamp.

**Faults** — Refresh button; clear-latched enabled only when latched faults present.

**Config** — Export Default Config button writes `BmsConfig()` defaults to a `.bin` file.

**Firmware Flash** — Protocol Update Simulation section:
- Enter Bootloader button (transitions fake target to bootloader mode)
- Run Simulation: loads the selected package, runs begin/chunk/finalize against the bootloader
- Progress bar shows chunk delivery; Abort cancels mid-flight
- Execute Flash (ST-Link, hardware required) remains behind safety checkbox

Safety enforced in GUI:
- Bring-Up and Firmware Flash tabs also enabled in BOOTLOADER mode
- All other runtime tabs disabled until `BMS_APP` mode confirmed
- Config/update buttons gated on hw_profile match
- Flash execute requires explicit safety checkbox
- No output-force or balancing-force buttons anywhere

**Native app packaging:** The GUI runs from source via `./scripts/run_gui.sh`.
PyInstaller bundling is not yet implemented.

## Static Fake Target Modes (12)

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
| `openwire_detected` | open-wire scan returns cell[0] flagged; status=0 |
| `openwire_pec_fail` | open-wire scan returns status=1 (PEC failure) |

## Live Simulator Modes (10)

Values change over time. One shared instance per server (unlike the static fake target
which creates a fresh instance per TCP connection).

```bash
# Start live simulator on port 65103 (default)
./scripts/run_fake_hardware.sh --mode healthy-idle
./scripts/run_fake_hardware.sh --mode drive          # cells draining
./scripts/run_fake_hardware.sh --mode cell-uv        # UV fault builds up
./scripts/run_fake_hardware.sh --mode temp-high      # temps rising
./scripts/run_fake_hardware.sh --seed 42             # deterministic drift
```

Connect the GUI: set host=127.0.0.1 port=65103 in the Connection tab.

| Mode | Description |
|------|-------------|
| `healthy-idle` | Cells ±5 mV random drift around 3700 mV; no faults |
| `drive` | Cells drain 1 mV per 2 ticks; temps at 28°C |
| `charge` | Cells charge 1 mV per 2 ticks from 3600 mV |
| `cell-uv` | cell[0] drifts down; FAULT_CELL_UV triggers at 2500 mV |
| `cell-ov` | cell[0] drifts up; FAULT_CELL_OV triggers at 4200 mV |
| `temp-high` | All temps rise 0.1°C per tick; plateau at 45°C |
| `isospi-fault` | Static FAULT_ISOSPI_CELL; cells and temps valid |
| `openwire-detected` | cell[0] flagged as open wire (scan result only) |
| `vpack-invalid` | Static FAULT_VPACK_INVALID |
| `bootloader` | Responds as FIRMWARE_TYPE_BOOTLOADER |

## Tests

```bash
python3 -m pytest tool/tests/ -q
# ~328 passed, 2 skipped (GUI tests skipped without PyQt6)
```

## Local Development

```bash
# One-command validation (no hardware needed)
./scripts/validate_all.sh

# Full stack demo (static fake target + CLI walkthrough)
./scripts/demo_local.sh

# Full stack demo with GUI
./scripts/demo_local.sh --gui
```

## Safety / Refusal Rules

| Condition | Effect |
|-----------|--------|
| No capabilities handshake | all ops refused |
| hw_profile_id mismatch | UNSUPPORTED mode; config/update refused |
| bootloader mode | runtime ops refused (values/cells/temps/faults) |
| BMS_APP mode | BOOT_UPDATE_* ops refused (0x0E error) |
| UNSUPPORTED mode | all config and update ops refused |
| ST-Link execute | requires `confirm=True` / explicit GUI checkbox |
| Package > 188 KB | PackageBuildError |
| Package empty | PackageBuildError |
| Wrong hw_profile in package | BEGIN rejected; validate_package_against_target fails |

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

**Static (`fake_target.py`):**
- `FakeTarget.serve_tcp()` creates a fresh `FakeTarget` per TCP connection — stateless.
- `FakeTargetInProcess` wraps a `FakeTarget` for synchronous in-process testing.
- Values are fixed at construction; no time evolution.
- 12 modes; used for all unit and integration tests.

**Live (`live_simulator.py`):**
- `LiveFakeHardware.serve_tcp()` creates **one shared instance** for all TCP connections.
- Background tick thread (200 ms default) evolves cell voltages, temperatures, and uptime.
- All state access is protected by a `threading.Lock`.
- Each TCP connection gets its own `FrameDecoder`; shared state goes through the lock.
- 10 live modes; seed parameter makes drift deterministic.
- Used for GUI demos and time-series visualisation; not used in pytest suite.

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

## Test Approach

1. `pytest tests/test_framing.py` — packet encode/decode, CRC, error injection
2. `pytest tests/test_config_validator.py` — client-side validation matches firmware
3. `pytest tests/test_fake_target.py` — full protocol flows against FakeTarget
4. `pytest tests/test_live_simulator.py` — evolving-state modes, thread safety, determinism
5. `pytest tests/test_package_parser.py` — package header parse, bad-header detection
6. `pytest tests/test_bootloader_update.py` — full bootloader update flow simulation

All tests run without hardware.
