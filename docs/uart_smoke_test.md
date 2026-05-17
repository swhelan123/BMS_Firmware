# UART / CLI Smoke Test — BMS First-Flash

This document describes how to verify that the BMS firmware is responding correctly over
the UART link using the desktop CLI tool.

---

## 1. Physical Interface

| Parameter | Value |
|---|---|
| MCU UART | USART2 (PA2=TX, PA3=RX) |
| Bridge chip | CP2104 USB-to-UART (on-board) |
| Baud rate | **115200** |
| Format | 8N1 (8 data bits, no parity, 1 stop bit) |
| Flow control | None |
| Protocol SOF | `0xAA 0x55` (every frame begins with these two bytes) |

The CP2104 bridge presents a virtual serial port on the host machine. No separate driver
is needed on macOS (Catalina+). On Linux, the driver is `cp210x` (kernel module).

---

## 2. Finding the Serial Port

### macOS

```bash
# CP2104 typically appears as one of:
ls /dev/tty.usbserial-*
ls /dev/tty.SLAB_USBtoUART*

# Example output:
# /dev/tty.usbserial-0001
```

If the port does not appear, check:
1. USB cable is plugged in and supplies data (not charge-only).
2. CP2104 is powered (board supply present).
3. `system_profiler SPUSBDataType | grep -A5 CP210` to confirm USB enumeration.

### Linux

```bash
ls /dev/ttyUSB*
# or
dmesg | tail -20 | grep tty
```

---

## 3. Connecting with bmsctl

All commands below use `--serial PORT`. Replace `/dev/tty.usbserial-XXXX` with your port.

```bash
# Activate .venv first (or use the wrapper script)
source .venv/bin/activate
# or
./scripts/bmsctl.sh <command> --serial /dev/tty.usbserial-XXXX
```

### Quick-connect test

```bash
./scripts/bmsctl.sh connect --serial /dev/tty.usbserial-XXXX
```

Expected output (BMS application firmware):

```
  mode:                  BMS_APP
  firmware_version:      0.1.0
  hw_profile_id:         0x0001
  protocol_version:      1
  config_schema_version: 1
  cell_count:            75
  feature_flags:         0x00000017
```

`feature_flags: 0x00000017` decodes as:
- bit 0 (`0x01`): FEAT_CELL_VOLTAGE
- bit 1 (`0x02`): FEAT_TEMPERATURE
- bit 2 (`0x04`): FEAT_BALANCING
- bit 4 (`0x10`): FEAT_CAN

---

## 4. Expected Capabilities Response Fields

| Field | Expected Value | Notes |
|---|---|---|
| `mode` | `BMS_APP` | Confirms application (not bootloader) is running |
| `firmware_version` | `0.1.0` (or current build) | Set in `bms_constants.h` `FW_VERSION_*` |
| `hw_profile_id` | `0x0001` | Must match `HW_PROFILE_ID` in firmware |
| `protocol_version` | `1` | Must match `PROTOCOL_VERSION` |
| `config_schema_version` | `1` | Must match `CONFIG_SCHEMA_VERSION` |
| `cell_count` | `75` | 5 × LTC6812 × 15 channels |
| `feature_flags` | `0x00000017` | Cell voltage + temp + balancing + CAN |

If `mode` is `BOOTLOADER`: the MCU booted into the BMS protocol bootloader. The
application did not start. See §6 — Failure Mode: Wrong Firmware Target.

---

## 5. Expected Diagnostics Response Fields

```bash
./scripts/bmsctl.sh diagnostics --serial /dev/tty.usbserial-XXXX
```

Expected output on first boot (no sensors connected):

```
  reset_cause:     0x04       # IWDG or POR — see below
  pec_cell_errors: 0          # or non-zero if measurement attempts failed
  pec_temp_errors: 0          # same
  i2c_errors:      0          # or non-zero if ISL28022 absent
  open_wire_valid: False
  open_wire_mask:  0000...    # all zeros — no scan run yet
  uptime_ms:       2345       # > 0 confirms main loop is running
```

`reset_cause` bit field (from STM32 RCC CSR):
- `0x04`: NRST (pin reset) — normal first power-on
- `0x08`: IWDG reset (watchdog fired) — indicates a prior blocking fault
- `0x10`: WWDG reset
- `0x20`: Software reset
- `0x40`: Low-power reset

Non-zero `pec_cell_errors` or `pec_temp_errors` are expected on first boot without the
LTC6812 chain connected. Each measurement attempt that fails due to missing hardware will
increment these counters.

Non-zero `i2c_errors` is expected without the ISL28022 connected.

---

## 6. TCP / Fake Target vs Serial Comparison

The tool supports both TCP (fake target, no hardware) and serial (real hardware). Use TCP
to verify the tool stack independently of hardware.

### TCP (fake target — no hardware required)

```bash
# Terminal 1: start fake target
./scripts/bmsctl.sh fake-target run --mode healthy

# Terminal 2: connect via TCP
./scripts/bmsctl.sh connect
./scripts/bmsctl.sh diagnostics
```

Expected: all values valid, no faults, 75 healthy cells.

### Serial (real hardware)

```bash
./scripts/bmsctl.sh connect --serial /dev/tty.usbserial-XXXX
./scripts/bmsctl.sh diagnostics --serial /dev/tty.usbserial-XXXX
```

Use the fake target output as a reference for what "healthy" should look like. Differences
on real hardware are expected (measurement faults until sensors are connected).

---

## 7. Failure Modes and Diagnosis

### Transport failure — no connection at all

**Symptom:** `bmsctl connect --serial PORT` prints:
```
error: cannot connect — [Errno 2] No such file or directory: '/dev/tty.usbserial-XXXX'
```

**Cause:** Port does not exist — CP2104 not enumerated.
**Fix:** Verify USB cable, board power, CP2104 VCC. See §2.

---

**Symptom:** `bmsctl connect --serial PORT` prints:
```
error: cannot connect — [Errno 16] Resource busy: '/dev/tty.usbserial-XXXX'
```

**Cause:** Another process (screen, minicom, another bmsctl) holds the port.
**Fix:** Close other terminal sessions. `lsof /dev/tty.usbserial-XXXX` to find the holder.

---

### Transport failure — port opens but no response

**Symptom:** `bmsctl connect` hangs for ~2 seconds then prints:
```
error: timeout waiting for capabilities response
```

**Cause:** Either no firmware running (MCU in boot loop or factory bootloader), or the
UART wiring is incorrect (TX/RX swapped, wrong port).

**Diagnosis steps:**
1. Verify the port is the correct one (not a different USB device).
2. Open the port in a terminal emulator at 115200 and watch for any output:
   ```bash
   screen /dev/tty.usbserial-XXXX 115200
   # or
   python3 -c "
   import serial, time
   s = serial.Serial('/dev/tty.usbserial-XXXX', 115200, timeout=2)
   time.sleep(2)
   print(s.read(64).hex())
   "
   ```
   If you see `aa55...` patterns: firmware is running and sending BMS frames.
   If you see garbage: baud rate wrong or TX/RX swapped.
   If you see nothing: MCU is not transmitting.

---

### Protocol framing failure

**Symptom:** Port opens, bytes appear, but `bmsctl connect` still times out or errors.

**Cause:** Firmware is sending output that is not BMS protocol framing. Could be a debug
UART print, a different firmware, or a truncated frame.

**Diagnosis:**
1. Capture raw bytes as above and inspect the hex.
2. Valid BMS frames start with `aa 55`. If the first bytes are not `aa 55`, the wrong
   firmware is running or the firmware is printing debug text before initialising the
   protocol layer.
3. Check `bms_protocol_init()` is called before any debug prints in `main()`.

---

### Wrong firmware target

**Symptom:** `bmsctl connect` exits 0 but prints `mode: BOOTLOADER`.

**Cause:** The MCU booted into the BMS protocol bootloader instead of the application.
This means either:
- The application was not successfully flashed (verify flash succeeded with `-v`).
- The application is crashing immediately and the bootloader is falling through.
- The RTC boot-entry flag (`BL_ENTRY_FLAG = 0xB007B007`) is set in the backup register
  from a prior update attempt.

**Fix:**
1. Verify `build_firmware/firmware.bin` was written correctly — re-flash with verify:
   `./scripts/flash_stlink.sh --app build_firmware/firmware.bin --execute`
2. Connect via SWD and erase the RTC backup register (RTC_BKP0R at 0x40002850) if needed.
3. Power-cycle the board.

---

### Hardware probe failure (isoSPI / I2C)

**Symptom:** `bmsctl probe cell-chain` returns `status: FAIL  ic_count: 0`.

**Cause:** isoSPI chain not responding. See `docs/first_flash_guide.md §9` — CELL probe
recovery.

**Symptom:** `bmsctl probe isl` returns `status: FAIL (I2C NACK)`.

**Cause:** ISL28022 at address `0x40` not responding. Verify I2C address and wiring. See
`docs/first_flash_guide.md §9` — ISL recovery.

---

### Distinguishing failure types (summary)

| Symptom | Most Likely Cause |
|---|---|
| Port not found | USB/CP2104 not enumerated |
| Port busy | Other process holding port |
| Port opens; no bytes at all | MCU not booting or UART TX wiring open |
| Bytes appear but not `aa 55` framing | Wrong firmware / debug print before protocol init |
| `aa 55` frames; timeout on capabilities | Protocol layer not responding; check `bms_protocol_poll()` in main loop |
| `mode: BOOTLOADER` | App not flashed or crashing at startup |
| `mode: BMS_APP`; probe failures | Hardware subsystems absent or wiring issue |
| `mode: BMS_APP`; outputs != 0 | Output init failure — stop and investigate |

---

*See also: [first_flash_guide.md](first_flash_guide.md),
[bench_safety_checklist.md](bench_safety_checklist.md),
[04_protocol_contract.md](04_protocol_contract.md)*
