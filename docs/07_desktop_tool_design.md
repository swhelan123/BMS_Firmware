# 07 — Desktop Tool Design

## 1. Recommended Implementation Stack

**Python + PyQt6 (or PySide6)**

| Factor | Rationale |
|---|---|
| Language | Python: fast iteration, rich serial/USB library ecosystem |
| UI framework | PyQt6: native-looking cross-platform GUI; mature; good table/chart widgets |
| Serial comms | `pyserial` for UART; straightforward, well-tested |
| ST-Link integration | Subprocess call to `STM32_Programmer_CLI`; avoids USB driver complexity |
| Packaging | Runs from Python source via `./scripts/run_gui.sh`; PyInstaller standalone bundling is not yet implemented |
| Alternative | Rust + egui/tauri if strong type safety preference; heavier build chain |

**Why not Electron/web-based:** Adds complexity for binary framing and serial I/O; native tool is simpler for this use case.

---

## 2. Target Modes

The tool is mode-aware at all times. Mode is determined from the `GET_CAPABILITIES` response.

| Mode | Trigger | Allowed Operations |
|---|---|---|
| `DISCONNECTED` | No connection open | Port selection only |
| `BMS_APP` | Capabilities confirm `firmware_type == 0x0001` | Full: monitoring, config, diagnostics, enter-bootloader |
| `BOOTLOADER` | Capabilities confirm `firmware_type == 0x0002` | Firmware update only; no runtime config |
| `UNSUPPORTED` | `hw_profile_id` unrecognized OR capabilities exchange fails | Display error; disconnect recommended |

---

## 3. Connection Flow

```
User selects port → tool opens port
  │
  ├─ Send GET_CAPABILITIES (retry up to 3×, 1s timeout each)
  │
  ├─ No response → mode: UNKNOWN_TARGET
  │
  ├─ Response: parse firmware_type, hw_profile_id, protocol_version
  │   ├─ protocol_version < TOOL_MIN_SUPPORTED → mode: UNSUPPORTED_TARGET
  │   ├─ hw_profile_id not in known list → mode: UNSUPPORTED_TARGET
  │   ├─ firmware_type == 0x0001 → mode: BMS_APPLICATION
  │   └─ firmware_type == 0x0002 → mode: BOOTLOADER
  │
  └─ Display mode banner on all pages; enable/disable pages accordingly
```

**Rule:** Tool must never assume device identity from port name. Capabilities response is mandatory.

---

## 4. UI Pages

### 4.1 Connection Page

- Serial port selector (auto-populated from system ports; refresh button)
- Baud rate selector (default 115200)
- Connect / Disconnect button
- Connection status indicator (color-coded: grey=disconnected, yellow=connecting, green=connected, red=error)
- Device info panel (populated after capabilities): firmware version, hw profile, protocol version, schema version, feature flags
- Mode banner (BMS Application / Bootloader / Unknown / Unsupported)

---

### 4.2 Dashboard Page

Available in: `BMS_APPLICATION`

- Pack summary: Vbat (mV), Vpack (mV), I_batt (mA → A), State, Uptime
- Permission outputs status: four indicator LEDs (MasterOk, DischargePermission, ChargePermission, ChargerSafety) — green=asserted, red=deasserted
- Active fault count: prominent badge; click navigates to Faults page
- Measurement validity flags: cells valid, temps valid, Vbat valid, Vpack valid
- Auto-refresh rate: configurable (default 500 ms via GET_VALUES)
- Mini-charts: last 60 s of Vbat, I_batt (rolling buffer, client-side)

---

### 4.3 Cells Page

Available in: `BMS_APPLICATION`

- 75-cell grid display (5 × 15 table layout corresponding to IC layout)
- Each cell: voltage in mV, color-coded:
  - Green: normal range
  - Yellow: soft OV/UV warning
  - Red: hard OV/UV fault
  - Grey: invalid/stale
- Min / Max / Delta summary row
- Balance status indicator per cell (if balancing enabled): show which cells are actively being discharged
- Refresh on demand or continuous (configurable)
- Export to CSV button

---

### 4.4 Temperatures Page

Available in: `BMS_APPLICATION`

- 75-sensor grid display (5 × 15 table)
- Each sensor: temperature in °C, color-coded:
  - Green: within all limits
  - Yellow: warning threshold
  - Red: hard limit or invalid
  - Grey: invalid
- Min / Max summary
- Threshold overlay lines on optional chart view
- Refresh rate configurable

---

### 4.5 Faults Page

Available in: `BMS_APPLICATION`

- Two fault tables: Active Faults and Latched Faults
- Each row: fault name, severity badge, description, timestamp of first occurrence
- Color-coded severity: INFO (white), WARNING (yellow), ERROR (orange), CRITICAL (red), FATAL (dark red)
- Clear Latched Faults button:
  - Enabled only when at least one latched fault has its active condition resolved
  - Sends `CLEAR_LATCHED_FAULTS` with mask of resolved faults
  - Prompts user to confirm before sending
- Fault history log (events from diagnostic ring buffer)

---

### 4.6 Config Page

Available in: `BMS_APPLICATION` (read also available in `BOOTLOADER` via GET_CONFIG if bootloader supports it)

**Sub-sections:**

**Read Config:**
- "Read from Device" button → `GET_CONFIG` → populates all fields
- "Load from File" button → open JSON/YAML config file → populate fields

**Edit Config:**
- Grouped QSpinBox / QLineEdit fields for all editable config fields; organized by section
- Fields show validation state inline
- Mask fields (`required_cell_mask`, `required_temp_mask`, `balance_allowed_mask`) edited as 20-character hex strings; validated for length and reserved-bit usage

**Validation:**
- Client-side validation on Apply (mirrors firmware validation logic)

**Apply:**
- "Apply to RAM" button → `SET_CONFIG_RAM` → config active until reset (for testing without persisting)
- Persistent `STORE_CONFIG` (write to flash) is not yet implemented in the GUI

**Raw View:**
- Read-only hex dump of the current config blob as a secondary tab

**Rule:** Wrong hardware profile detected in loaded config → refuse Apply; show error.

---

### 4.7 Diagnostics / Terminal Page

Available in: `BMS_APPLICATION`, `UNKNOWN_TARGET` (terminal only)

**Diagnostics sub-tab:**
- PEC error counters (CELL/TEMP chain, I2C errors)
- Open-wire result: 75-cell grid showing last open-wire result; "Run Open Wire Test" button
- Reset cause: last reset reason string
- Self-test results (if run)

**Terminal sub-tab:**
- Raw UART terminal (hex + ASCII display)
- Send raw bytes capability (for development)
- Protocol log: show all frames sent/received with parsed packet ID and summary

**Log export:** Save terminal content to file.

---

### 4.8 Firmware Flash Page

Available in: all modes (different sub-sections per mode)

**ST-Link Development Flash (always available if ST-Link detected):**
- Detect ST-Link button (calls `STM32_Programmer_CLI --list`)
- Select .bin or .elf file
- Flash address (default 0x08000000 for full image, or split addresses)
- "Flash via ST-Link" button → runs `STM32_Programmer_CLI` as subprocess
- Output terminal shows stdout/stderr from programmer
- Progress indicator
- Verify after flash option

**Bootloader Package Update (available when `FEAT_BOOTLOADER` in capabilities, or in `BOOTLOADER` mode):**
- Select .pkg firmware package file
- Parse and display package header info: firmware version, hw profile, schema version, image size, CRC
- Hardware profile match indicator: green/red
- Schema version change warning (if different from device)
- "Enter Bootloader" button → sends `ENTER_BOOTLOADER`; waits for reconnect in bootloader mode (10s timeout)
- Update sequence: `BOOT_UPDATE_BEGIN` → chunk loop (`BOOT_UPDATE_CHUNK`) → `BOOT_UPDATE_FINALIZE`
- Progress bar (chunk_index / total_chunks)
- Abort button → `BOOT_UPDATE_ABORT`

**Rules:**
- Tool refuses to flash package if `hw_profile_id` mismatch
- Tool refuses to flash package if not in `BOOTLOADER` mode after `ENTER_BOOTLOADER`
- Tool prompts user that config may need re-validation if schema version changes

---

### 4.9 Logs Page

Available in: all modes

- Persistent log of all tool events (connection, disconnection, config reads/writes, flash operations)
- Timestamped entries
- Filter by severity
- Export to file
- Automatic rotation (last 10,000 entries in-memory; exportable before clear)

---

## 5. Capabilities-First Enforcement

| Action | Requires |
|---|---|
| Any config read | Valid capabilities response |
| VALIDATE_CONFIG | Capabilities + `BMS_APPLICATION` mode |
| SET_CONFIG_RAM | Capabilities + `BMS_APPLICATION` mode |
| STORE_CONFIG | Capabilities + `BMS_APPLICATION` mode + prior successful VALIDATE_CONFIG |
| ENTER_BOOTLOADER | Capabilities + `BMS_APPLICATION` mode |
| BOOT_UPDATE_BEGIN | Capabilities + `BOOTLOADER` mode |
| BOOT_UPDATE_CHUNK | Active update session started by BEGIN |
| BOOT_UPDATE_FINALIZE | All chunks sent |

The tool maintains a `CapabilitiesState` object that pages query before enabling buttons.

---

## 6. Fake Target / Simulator

A fake target simulates a BMS device over a virtual serial port (or TCP socket). Used for:
- Tool development without hardware
- Protocol golden tests
- CI testing of tool UI flows

`fake_target.py` implements the full request/response protocol for all defined packets. It can inject faults, simulate cell voltages, and simulate bootloader mode. Modes are hardcoded enum values — there is no YAML scenario file; the mode is selected at startup via `--mode`.

`live_simulator.py` provides a live simulator with time-evolving values (cell drain, temperature rise, fault build-up). One shared instance per TCP server; 10 modes; started via `./scripts/run_fake_hardware.sh`.

---

## 7. Config Editor Strategy

- Config fields are defined in `protocol/config_schema.yaml` (machine-readable; authoritative source of truth)
- The tool UI config editor is **hardcoded** with QSpinBox/QLineEdit widgets for each field; it is not auto-generated from the YAML
- When a new config field is added, both `config_schema.yaml` and the editor widgets in `tool/src/gui/pages/config.py` must be updated manually
- The schema YAML is used for documentation and for the CLI `config export-json` / `config diff` paths, not for UI generation

---

## 8. Packaging

The tool runs from Python source. Launch via the provided shell script (activates `.venv` automatically):

```bash
./scripts/run_gui.sh                          # open GUI, connect manually
./scripts/run_gui.sh --fake --mode healthy    # auto-connect to fake target
```

PyInstaller standalone bundling (macOS/Windows/Linux single-file executable) is **not yet implemented**.

A source-based release bundle is generated by `./scripts/package_release.sh` — this copies firmware artifacts, tool source, scripts, and docs into `dist/bms-v{VERSION}/`. No `.venv` or `__pycache__` is included.

---

## 9. What the Tool Must Refuse to Do

| Prohibited Action | Reason |
|---|---|
| Send STORE_CONFIG without successful validation | Could brick device config |
| Send BOOT_UPDATE_BEGIN before GET_CAPABILITIES | Could flash wrong hardware |
| Flash firmware package with mismatched hw_profile_id | Hardware damage risk |
| Send config with schema version the device does not support | Silent field misinterpretation |
| Claim successful update without FINALIZE CRC verification | Silent incomplete flash |
| Bypass mode check to send BMS commands in bootloader mode | Undefined behaviour |
| Assert any safety permission directly (tool has no such mechanism) | Not applicable; tool is read/config only |
