"""fake_target.py — simulated BMS device for tool development and CI tests.

Implements the full BMS request/response protocol over a serial-like interface.
Can run as:
  - TCP server: FakeTarget.serve_tcp(host, port)
  - In-process mock: FakeTargetInProcess
  - Serial subprocess: run_on_serial_port()

Simulation modes:
  - 'healthy'           — nominal voltages/temps, no faults
  - 'safe_invalid'      — all measurements invalid (MEAS_ERROR), no faults
  - 'cell_uv'           — cell[0] at 2400 mV, FAULT_CELL_UV active
  - 'cell_ov'           — cell[0] at 4300 mV, FAULT_CELL_OV active
  - 'temp_invalid'      — all temps INVALID, FAULT_TEMP_READ_INVALID active
  - 'vpack_invalid'     — FAULT_VPACK_INVALID active
  - 'isospi_fault'      — FAULT_ISOSPI_CELL active
  - 'config_error'      — FAULT_CONFIG_INVALID active
  - 'overcurrent_fault' — FAULT_OVERCURRENT active (latching)
  - 'bootloader'        — capabilities report FIRMWARE_TYPE_BOOTLOADER
"""
import socket
import struct
import threading
import io
from typing import Optional

from ..protocol.framing import encode_frame, FrameDecoder
from ..protocol.packet_defs import (
    PKT_GET_CAPABILITIES, PKT_GET_VALUES, PKT_GET_CELLS, PKT_GET_TEMPS,
    PKT_GET_TEMPS_RAW,
    PKT_GET_FAULTS, PKT_CLEAR_LATCHED_FAULTS, PKT_GET_CHARGER_STATUS,
    PKT_GET_CONFIG, PKT_VALIDATE_CONFIG, PKT_SET_CONFIG_RAM, PKT_STORE_CONFIG,
    PKT_GET_DIAGNOSTICS_SUMMARY, PKT_RUN_OPENWIRE,
    PKT_GET_BOOT_INFO, PKT_ENTER_BOOTLOADER,
    PKT_BOOT_UPDATE_BEGIN, PKT_BOOT_UPDATE_CHUNK,
    PKT_BOOT_UPDATE_FINALIZE, PKT_BOOT_UPDATE_ABORT,
    PKT_GET_GPIO_SNAPSHOT, PKT_GET_OUTPUTS_SNAPSHOT,
    PKT_PROBE_CELL_CHAIN, PKT_PROBE_TEMP_CHAIN,
    PKT_PROBE_ISL28022, PKT_READ_VPACK_RAW, PKT_BALANCE_DISABLE_ALL,
    PKT_MEASURE_CELLS_ONCE, PKT_MEASURE_TEMPS_ONCE, PKT_MEASURE_POWER_ONCE,
    PROTOCOL_VERSION, HW_PROFILE_ID, CONFIG_SCHEMA_SIZE,
    FIRMWARE_TYPE_BMS_APP, FIRMWARE_TYPE_BOOTLOADER,
    TOTAL_CELL_COUNT, TOTAL_TEMP_COUNT,
)
from ..protocol.bms_defs import (
    FAULT_BIT_CELL_OV, FAULT_BIT_CELL_UV, FAULT_BIT_TEMP_READ_INVALID,
    FAULT_BIT_VPACK_INVALID, FAULT_BIT_ISOSPI_CELL, FAULT_BIT_ISOSPI_TEMP,
    FAULT_BIT_I2C_ISL28022, FAULT_BIT_CONFIG_INVALID, FAULT_BIT_OVERCURRENT,
    BMS_STATE_STANDBY,
)
from ..config.schema import BmsConfig

TEMP_INVALID_CX10 = -0x8000  # sentinel

BL_ENTRY_FLAG = 0xB007B007

# ── Simulation mode presets ───────────────────────────────────────────────────

_KNOWN_MODES = frozenset([
    'healthy', 'safe_invalid', 'cell_uv', 'cell_ov', 'temp_invalid',
    'vpack_invalid', 'isospi_fault', 'config_error', 'overcurrent_fault',
    'bootloader', 'openwire_detected', 'openwire_pec_fail',
])


def _apply_simulation_mode(target: 'FakeTarget', mode: str) -> None:
    """Configure FakeTarget state for a named simulation mode."""
    if mode == 'healthy':
        pass  # defaults are healthy
    elif mode == 'safe_invalid':
        # Measurements not yet valid — no active faults, but no good data either
        target.set_cell_mv([0] * TOTAL_CELL_COUNT)
        target.set_temps_cx10([TEMP_INVALID_CX10] * TOTAL_TEMP_COUNT)
    elif mode == 'cell_uv':
        cell_mv = [3700] * TOTAL_CELL_COUNT
        cell_mv[0] = 2400
        target.set_cell_mv(cell_mv)
        target.inject_fault(FAULT_BIT_CELL_UV)
    elif mode == 'cell_ov':
        cell_mv = [3700] * TOTAL_CELL_COUNT
        cell_mv[0] = 4300
        target.set_cell_mv(cell_mv)
        target.inject_fault(FAULT_BIT_CELL_OV)
    elif mode == 'temp_invalid':
        target.set_temps_cx10([TEMP_INVALID_CX10] * TOTAL_TEMP_COUNT)
        target.inject_fault(FAULT_BIT_TEMP_READ_INVALID)
    elif mode == 'vpack_invalid':
        target.inject_fault(FAULT_BIT_VPACK_INVALID)
    elif mode == 'isospi_fault':
        target.inject_fault(FAULT_BIT_ISOSPI_CELL)
    elif mode == 'config_error':
        target.inject_fault(FAULT_BIT_CONFIG_INVALID)
    elif mode == 'overcurrent_fault':
        target.inject_fault(FAULT_BIT_OVERCURRENT)
    elif mode == 'bootloader':
        target.set_firmware_type(FIRMWARE_TYPE_BOOTLOADER)
    elif mode == 'openwire_detected':
        detected = [False] * TOTAL_CELL_COUNT
        detected[0] = True  # cell 0 has an open wire
        target.set_open_wire(valid=True, detected=detected)
    elif mode == 'openwire_pec_fail':
        target._openwire_pec_fail = True
    else:
        raise ValueError(f"Unknown simulation mode: {mode!r}")


# ── Core fake target ──────────────────────────────────────────────────────────

class FakeTarget:
    """Core fake target state machine. Feed bytes in, get response bytes out."""

    def __init__(self, mode: str = 'healthy'):
        self._decoder = FrameDecoder()
        self._config = BmsConfig()
        self._active_faults   = 0
        self._latched_faults  = 0
        self._cell_mv         = [3700] * TOTAL_CELL_COUNT
        self._temps_cx10      = [250]  * TOTAL_TEMP_COUNT
        self._uptime_ms       = 0
        self._pec_errors_cell = 0
        self._pec_errors_temp = 0
        self._i2c_errors      = 0
        self._open_wire_valid    = False
        self._open_wire_detected = [False] * TOTAL_CELL_COUNT
        self._openwire_pec_fail  = False
        self._reset_cause     = 0x01  # POR on fresh boot
        self._firmware_type   = FIRMWARE_TYPE_BMS_APP
        self._soc_pct_x10     = 750   # 75.0% initial SOC
        # Bootloader update state
        self._update_state          = None    # None | 'accepting_chunks' | 'complete'
        self._update_staged         = bytearray()
        self._update_chunk_size     = 512
        self._update_total_chunks   = 0
        self._update_next_chunk_idx = 0
        # Bring-up state
        self._gpio_cs_cell          = 1    # idle = high
        self._gpio_cs_temp          = 1
        self._gpio_power_button     = 0
        self._gpio_charge_detect    = 0
        self._gpio_power_enable     = 1
        self._gpio_outputs          = 0    # logical permission bitmask
        self._isl_config_reg        = 0x599F   # matches firmware CFG_VALUE: BRNG=60V, PG=/8, 12-bit, continuous
        self._vpack_raw             = 2048     # ~50% of 4096, ~1.65 V
        _apply_simulation_mode(self, mode)

    # ── TCP server ────────────────────────────────────────────────────────────

    @classmethod
    def serve_tcp(cls, host: str = '127.0.0.1', port: int = 65102,
                  mode: str = 'healthy') -> None:
        """Block and serve connections; spawns a thread per client with a fresh FakeTarget."""
        server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        server.bind((host, port))
        server.listen(5)
        print(f"[fake_target] listening on {host}:{port}  mode={mode}", flush=True)
        while True:
            conn, addr = server.accept()
            print(f"[fake_target] client {addr}", flush=True)
            t = threading.Thread(target=cls._handle_tcp_client,
                                 args=(conn, mode), daemon=True)
            t.start()

    @classmethod
    def _handle_tcp_client(cls, conn, mode: str) -> None:
        target = cls(mode=mode)
        try:
            while True:
                data = conn.recv(4096)
                if not data:
                    break
                resp = target.feed(data)
                if resp:
                    conn.sendall(resp)
        except Exception:
            pass
        finally:
            conn.close()

    def feed(self, data: bytes) -> bytes:
        """Feed incoming bytes; returns all response bytes."""
        frames = self._decoder.feed(data)
        out = b''
        for f in frames:
            out += self._handle(f)
        return out

    def _respond(self, pkt_id: int, payload: bytes, seq: int,
                 is_error: bool = False) -> bytes:
        return encode_frame(pkt_id, payload, seq=seq, is_response=True, is_error=is_error)

    def _error(self, pkt_id: int, seq: int, code: int) -> bytes:
        return self._respond(pkt_id, bytes([code]), seq, is_error=True)

    def _handle(self, frame: dict) -> bytes:
        pkt_id  = frame['pkt_id']
        seq     = frame['seq']
        payload = frame['payload']

        dispatch = {
            PKT_GET_CAPABILITIES:       lambda: self._h_capabilities(seq),
            PKT_GET_VALUES:             lambda: self._h_values(seq),
            PKT_GET_CELLS:              lambda: self._h_cells(seq, payload),
            PKT_GET_TEMPS:              lambda: self._h_temps(seq),
            PKT_GET_TEMPS_RAW:          lambda: self._h_temps_raw(seq),
            PKT_GET_FAULTS:             lambda: self._h_faults(seq),
            PKT_CLEAR_LATCHED_FAULTS:   lambda: self._h_clear_latched(seq, payload),
            PKT_GET_CHARGER_STATUS:     lambda: self._h_charger_status(seq),
            PKT_GET_CONFIG:             lambda: self._h_get_config(seq),
            PKT_VALIDATE_CONFIG:        lambda: self._h_validate_config(seq, payload),
            PKT_SET_CONFIG_RAM:         lambda: self._h_set_config_ram(seq, payload),
            PKT_STORE_CONFIG:           lambda: self._h_store_config(seq, payload),
            PKT_GET_DIAGNOSTICS_SUMMARY: lambda: self._h_diagnostics_summary(seq),
            PKT_RUN_OPENWIRE:           lambda: self._h_run_openwire(seq),
            PKT_GET_GPIO_SNAPSHOT:      lambda: self._h_get_gpio_snapshot(seq),
            PKT_GET_OUTPUTS_SNAPSHOT:   lambda: self._h_get_outputs_snapshot(seq),
            PKT_PROBE_CELL_CHAIN:       lambda: self._h_probe_cell_chain(seq),
            PKT_PROBE_TEMP_CHAIN:       lambda: self._h_probe_temp_chain(seq),
            PKT_PROBE_ISL28022:         lambda: self._h_probe_isl28022(seq),
            PKT_READ_VPACK_RAW:         lambda: self._h_read_vpack_raw(seq),
            PKT_BALANCE_DISABLE_ALL:    lambda: self._h_balance_disable_all(seq),
            PKT_MEASURE_CELLS_ONCE:     lambda: self._h_measure_cells_once(seq),
            PKT_MEASURE_TEMPS_ONCE:     lambda: self._h_measure_temps_once(seq),
            PKT_MEASURE_POWER_ONCE:     lambda: self._h_measure_power_once(seq),
            PKT_GET_BOOT_INFO:          lambda: self._h_boot_info(seq),
            PKT_ENTER_BOOTLOADER:       lambda: self._h_enter_bootloader(seq, payload),
            PKT_BOOT_UPDATE_BEGIN:      lambda: self._h_boot_update_begin(seq, payload),
            PKT_BOOT_UPDATE_CHUNK:      lambda: self._h_boot_update_chunk(seq, payload),
            PKT_BOOT_UPDATE_FINALIZE:   lambda: self._h_boot_update_finalize(seq),
            PKT_BOOT_UPDATE_ABORT:      lambda: self._h_boot_update_abort(seq),
        }
        handler = dispatch.get(pkt_id)
        if handler is None:
            return self._error(pkt_id, seq, 0x01)
        return handler()

    # ── Packet handlers ───────────────────────────────────────────────────────

    def _h_capabilities(self, seq: int) -> bytes:
        resp = bytearray(26)
        struct.pack_into('<H', resp, 0,  self._firmware_type)
        resp[2]  = 0   # major
        resp[3]  = 1   # minor
        resp[4]  = 0   # patch
        struct.pack_into('<H', resp, 5,  HW_PROFILE_ID)
        struct.pack_into('<H', resp, 7,  PROTOCOL_VERSION)
        struct.pack_into('<H', resp, 9,  1)   # config_schema_version
        resp[11] = TOTAL_CELL_COUNT & 0xFF
        resp[12] = TOTAL_TEMP_COUNT & 0xFF
        struct.pack_into('<I', resp, 13, 0x07)   # feature flags: cell+temp+balance
        resp[17] = 9   # max_payload_log2
        struct.pack_into('<I', resp, 18, 186 * 1024)  # usable app size (region minus BL metadata page)
        struct.pack_into('<I', resp, 22, 8 * 1024)    # config_slot_size
        return self._respond(PKT_GET_CAPABILITIES, bytes(resp), seq)

    def _h_values(self, seq: int) -> bytes:
        i2c_fault   = bool((self._active_faults >> FAULT_BIT_I2C_ISL28022) & 1)
        vpack_fault = bool((self._active_faults >> FAULT_BIT_VPACK_INVALID) & 1)
        vbat_mv   = 0 if i2c_fault   else 48000
        vpack_mv  = 0 if vpack_fault else 48000
        i_batt_ma = 0
        meas_flags = (
            (0 if i2c_fault   else 1) |   # bit0: vbat_valid
            (0 if vpack_fault else 2) |   # bit1: vpack_valid
            (0 if i2c_fault   else 4)     # bit2: i_batt_valid
        )
        resp = bytearray(38)
        struct.pack_into('<i', resp, 0,  vbat_mv)
        struct.pack_into('<i', resp, 4,  vpack_mv)
        struct.pack_into('<i', resp, 8,  i_batt_ma)
        struct.pack_into('<H', resp, 12, BMS_STATE_STANDBY)
        struct.pack_into('<Q', resp, 14, self._active_faults)
        struct.pack_into('<Q', resp, 22, self._latched_faults)
        resp[30] = 0                                 # outputs_state
        struct.pack_into('<I', resp, 31, self._uptime_ms)
        resp[35] = meas_flags
        struct.pack_into('<h', resp, 36, self._soc_pct_x10)
        return self._respond(PKT_GET_VALUES, bytes(resp), seq)

    def _h_cells(self, seq: int, payload: bytes) -> bytes:
        include_validity = bool(payload and payload[0] & 1)
        resp = bytearray(156 + (10 if include_validity else 0))
        struct.pack_into('<H', resp, 0, TOTAL_CELL_COUNT)
        for i, mv in enumerate(self._cell_mv):
            struct.pack_into('<H', resp, 2 + i*2, mv & 0xFFFF)
        struct.pack_into('<I', resp, 152, self._uptime_ms)
        if include_validity:
            for i in range(TOTAL_CELL_COUNT):
                resp[156 + i//8] |= (1 << (i % 8))
        return self._respond(PKT_GET_CELLS, bytes(resp), seq)

    def _h_temps(self, seq: int) -> bytes:
        resp = bytearray(2 + TOTAL_TEMP_COUNT * 2)
        struct.pack_into('<H', resp, 0, TOTAL_TEMP_COUNT)
        for i, t in enumerate(self._temps_cx10):
            struct.pack_into('<h', resp, 2 + i*2, t & 0xFFFF)
        return self._respond(PKT_GET_TEMPS, bytes(resp), seq)

    def _h_temps_raw(self, seq: int) -> bytes:
        # Simulated raw C-input voltages: reverse the Enepaq conversion enough
        # to give a plausible in-window mV for valid channels, 0 for invalid.
        resp = bytearray(2 + TOTAL_TEMP_COUNT * 2)
        struct.pack_into('<H', resp, 0, TOTAL_TEMP_COUNT)
        for i, t in enumerate(self._temps_cx10):
            mv = 0 if (t & 0xFFFF) == 0x8000 else 1800
            struct.pack_into('<H', resp, 2 + i*2, mv)
        return self._respond(PKT_GET_TEMPS_RAW, bytes(resp), seq)

    def _h_faults(self, seq: int) -> bytes:
        resp = struct.pack('<QQ', self._active_faults, self._latched_faults)
        return self._respond(PKT_GET_FAULTS, resp, seq)

    def _h_charger_status(self, seq: int) -> bytes:
        # The fake target doesn't simulate a live Elcon CAN link — status_valid
        # stays False, matching real firmware before any status frame arrives.
        resp = struct.pack('<BHHBBI', 0, 0, 0, 0, 0, 0xFFFFFFFF)
        return self._respond(PKT_GET_CHARGER_STATUS, resp, seq)

    def _h_clear_latched(self, seq: int, payload: bytes) -> bytes:
        mask = struct.unpack_from('<Q', payload)[0] if len(payload) >= 8 else (2**64 - 1)
        clearable = mask & self._latched_faults & ~self._active_faults
        self._latched_faults &= ~clearable
        return self._respond(PKT_CLEAR_LATCHED_FAULTS, struct.pack('<Q', clearable), seq)

    def _h_get_config(self, seq: int) -> bytes:
        return self._respond(PKT_GET_CONFIG, self._config.pack(), seq)

    def _h_validate_config(self, seq: int, payload: bytes) -> bytes:
        from ..config.validator import validate_config
        if len(payload) != CONFIG_SCHEMA_SIZE:
            return self._error(PKT_VALIDATE_CONFIG, seq, 0x02)
        cfg = BmsConfig.unpack(payload)
        ok, err_off, _ = validate_config(cfg)
        return self._respond(PKT_VALIDATE_CONFIG, struct.pack('<BH', 0 if ok else 1, err_off), seq)

    def _h_set_config_ram(self, seq: int, payload: bytes) -> bytes:
        from ..config.validator import validate_config
        if len(payload) != CONFIG_SCHEMA_SIZE:
            return self._error(PKT_SET_CONFIG_RAM, seq, 0x02)
        cfg = BmsConfig.unpack(payload)
        ok, err_off, _ = validate_config(cfg)
        if ok:
            self._config = cfg
        return self._respond(PKT_SET_CONFIG_RAM, struct.pack('<BH', 0 if ok else 1, err_off), seq)

    def _h_store_config(self, seq: int, payload: bytes) -> bytes:
        """Mirrors firmware handle_store_config: validate, persist, bump
        generation. Response is a single status byte (0 = stored)."""
        from ..config.validator import validate_config
        if len(payload) != CONFIG_SCHEMA_SIZE:
            return self._error(PKT_STORE_CONFIG, seq, 0x02)
        cfg = BmsConfig.unpack(payload)
        ok, _, _ = validate_config(cfg)
        if not ok:
            return self._respond(PKT_STORE_CONFIG, bytes([1]), seq, is_error=True)
        cfg.config_generation = self._config.config_generation + 1
        self._config = cfg
        return self._respond(PKT_STORE_CONFIG, bytes([0]), seq)

    def _h_diagnostics_summary(self, seq: int) -> bytes:
        # Layout: reset_cause(1) + pec_cell(4) + pec_temp(4) + i2c(4) +
        #          open_wire_valid(1) + open_wire_mask(10) + uptime_ms(4) = 28 bytes
        resp = bytearray(28)
        resp[0] = self._reset_cause & 0xFF
        struct.pack_into('<I', resp, 1,  self._pec_errors_cell)
        struct.pack_into('<I', resp, 5,  self._pec_errors_temp)
        struct.pack_into('<I', resp, 9,  self._i2c_errors)
        resp[13] = 1 if self._open_wire_valid else 0
        for i, det in enumerate(self._open_wire_detected):
            if det:
                resp[14 + i // 8] |= (1 << (i % 8))
        struct.pack_into('<I', resp, 24, self._uptime_ms)
        return self._respond(PKT_GET_DIAGNOSTICS_SUMMARY, bytes(resp), seq)

    def _h_run_openwire(self, seq: int) -> bytes:
        if self._firmware_type != FIRMWARE_TYPE_BMS_APP:
            return self._error(PKT_RUN_OPENWIRE, seq, 0x0A)
        resp = bytearray(11)  # status(1) + mask(10)
        if self._openwire_pec_fail:
            resp[0] = 1  # PEC error during detection
        else:
            self._open_wire_valid = True
            resp[0] = 0
            for i, det in enumerate(self._open_wire_detected):
                if det:
                    resp[1 + i // 8] |= (1 << (i % 8))
        return self._respond(PKT_RUN_OPENWIRE, bytes(resp), seq)

    def _h_boot_info(self, seq: int) -> bytes:
        # Layout: fw_type(2)+major(1)+minor(1)+patch(1)+hw_profile_id(2)+
        #          proto_version(2)+reset_cause(1)+uptime_ms(4) = 14 bytes
        resp = bytearray(14)
        struct.pack_into('<H', resp, 0, FIRMWARE_TYPE_BMS_APP)
        resp[2]  = 0   # major
        resp[3]  = 1   # minor
        resp[4]  = 0   # patch
        struct.pack_into('<H', resp, 5,  HW_PROFILE_ID)
        struct.pack_into('<H', resp, 7,  PROTOCOL_VERSION)
        resp[9]  = self._reset_cause & 0xFF
        struct.pack_into('<I', resp, 10, self._uptime_ms)
        return self._respond(PKT_GET_BOOT_INFO, bytes(resp), seq)

    def _h_enter_bootloader(self, seq: int, payload: bytes) -> bytes:
        if len(payload) < 4:
            return self._error(PKT_ENTER_BOOTLOADER, seq, 0x02)
        magic = struct.unpack_from('<I', payload)[0]
        if magic != BL_ENTRY_FLAG:
            return self._error(PKT_ENTER_BOOTLOADER, seq, 0x0B)
        self._firmware_type = FIRMWARE_TYPE_BOOTLOADER
        self._update_state = None
        self._update_staged = bytearray()
        self._update_next_chunk_idx = 0
        return self._respond(PKT_ENTER_BOOTLOADER, b'', seq)

    def _h_boot_update_begin(self, seq: int, payload: bytes) -> bytes:
        if self._firmware_type != FIRMWARE_TYPE_BOOTLOADER:
            return self._error(PKT_BOOT_UPDATE_BEGIN, seq, 0x0E)  # not in bootloader
        if len(payload) < 64:
            return self._error(PKT_BOOT_UPDATE_BEGIN, seq, 0x02)
        from ..update.package_parser import parse_header, validate_header
        try:
            hdr = parse_header(payload[:64])
            validate_header(hdr, raw_header=bytes(payload[:64]))
        except Exception:
            resp = struct.pack('<BBII', 1, 0x01, 0, 0)  # rejected: bad header
            return self._respond(PKT_BOOT_UPDATE_BEGIN, resp, seq)
        total_chunks = (hdr.app_size + self._update_chunk_size - 1) // self._update_chunk_size
        self._update_staged = bytearray()
        self._update_total_chunks = total_chunks
        self._update_next_chunk_idx = 0
        self._update_expected_crc = hdr.app_crc32
        self._update_state = 'accepting_chunks'
        resp = struct.pack('<BBII', 0, 0, self._update_chunk_size, total_chunks)
        return self._respond(PKT_BOOT_UPDATE_BEGIN, resp, seq)

    def _h_boot_update_chunk(self, seq: int, payload: bytes) -> bytes:
        if self._firmware_type != FIRMWARE_TYPE_BOOTLOADER:
            return self._error(PKT_BOOT_UPDATE_CHUNK, seq, 0x0E)
        if self._update_state != 'accepting_chunks':
            return self._error(PKT_BOOT_UPDATE_CHUNK, seq, 0x0C)  # not started
        if len(payload) < 8:
            return self._error(PKT_BOOT_UPDATE_CHUNK, seq, 0x02)
        index, data_len = struct.unpack_from('<II', payload)
        data = payload[8:8 + data_len]
        if index != self._update_next_chunk_idx:
            return self._error(PKT_BOOT_UPDATE_CHUNK, seq, 0x0D)  # wrong index
        self._update_staged.extend(data)
        self._update_next_chunk_idx += 1
        return self._respond(PKT_BOOT_UPDATE_CHUNK, bytes([0]), seq)

    def _h_boot_update_finalize(self, seq: int) -> bytes:
        if self._firmware_type != FIRMWARE_TYPE_BOOTLOADER:
            return self._error(PKT_BOOT_UPDATE_FINALIZE, seq, 0x0E)
        if self._update_state != 'accepting_chunks':
            return self._error(PKT_BOOT_UPDATE_FINALIZE, seq, 0x0C)
        from ..protocol.crc import crc32_iso_hdlc
        computed = crc32_iso_hdlc(bytes(self._update_staged))
        # Mirror the real bootloader: FINALIZE fails if the written image's
        # CRC does not match the app_crc32 promised in the BEGIN header.
        if computed != getattr(self, '_update_expected_crc', computed):
            self._update_state = None
            return self._respond(PKT_BOOT_UPDATE_FINALIZE,
                                 struct.pack('<BI', 1, computed), seq,
                                 is_error=True)
        self._update_state = 'complete'
        return self._respond(PKT_BOOT_UPDATE_FINALIZE, struct.pack('<BI', 0, computed), seq)

    def _h_boot_update_abort(self, seq: int) -> bytes:
        self._update_state = None
        self._update_staged = bytearray()
        self._update_next_chunk_idx = 0
        return self._respond(PKT_BOOT_UPDATE_ABORT, b'', seq)

    # ── Bring-up handlers ─────────────────────────────────────────────────────

    def _h_get_gpio_snapshot(self, seq: int) -> bytes:
        if self._firmware_type != FIRMWARE_TYPE_BMS_APP:
            return self._error(PKT_GET_GPIO_SNAPSHOT, seq, 0x0A)
        resp = bytes([
            self._gpio_cs_cell, self._gpio_cs_temp,
            self._gpio_power_button, self._gpio_charge_detect,
            self._gpio_power_enable,
            (self._gpio_outputs >> 0) & 1,  # master_ok_raw
            (self._gpio_outputs >> 1) & 1,  # discharge_raw
            (self._gpio_outputs >> 2) & 1,  # charge_raw
            (self._gpio_outputs >> 3) & 1,  # charger_safety_raw
        ])
        return self._respond(PKT_GET_GPIO_SNAPSHOT, resp, seq)

    def _h_get_outputs_snapshot(self, seq: int) -> bytes:
        if self._firmware_type != FIRMWARE_TYPE_BMS_APP:
            return self._error(PKT_GET_OUTPUTS_SNAPSHOT, seq, 0x0A)
        logical = self._gpio_outputs & 0xFF
        raw     = self._gpio_outputs & 0xFF   # same in simulation
        return self._respond(PKT_GET_OUTPUTS_SNAPSHOT, bytes([logical, raw]), seq)

    def _h_probe_cell_chain(self, seq: int) -> bytes:
        if self._firmware_type != FIRMWARE_TYPE_BMS_APP:
            return self._error(PKT_PROBE_CELL_CHAIN, seq, 0x0A)
        is_fault = bool((self._active_faults >> FAULT_BIT_ISOSPI_CELL) & 1)
        ic_count = 5
        resp = bytearray(2 + ic_count * 7)
        resp[0] = 1 if is_fault else 0
        resp[1] = ic_count
        for i in range(ic_count):
            off = 2 + i * 7
            resp[off] = 0 if is_fault else 1   # responded
            if not is_fault:
                resp[off + 1] = 0xF8           # nominal CFGA[0]
        return self._respond(PKT_PROBE_CELL_CHAIN, bytes(resp), seq)

    def _h_probe_temp_chain(self, seq: int) -> bytes:
        if self._firmware_type != FIRMWARE_TYPE_BMS_APP:
            return self._error(PKT_PROBE_TEMP_CHAIN, seq, 0x0A)
        is_fault = bool((self._active_faults >> FAULT_BIT_ISOSPI_TEMP) & 1)
        ic_count = 5
        resp = bytearray(2 + ic_count * 7)
        resp[0] = 1 if is_fault else 0
        resp[1] = ic_count
        for i in range(ic_count):
            off = 2 + i * 7
            resp[off] = 0 if is_fault else 1
            if not is_fault:
                resp[off + 1] = 0xF8
        return self._respond(PKT_PROBE_TEMP_CHAIN, bytes(resp), seq)

    def _h_probe_isl28022(self, seq: int) -> bytes:
        if self._firmware_type != FIRMWARE_TYPE_BMS_APP:
            return self._error(PKT_PROBE_ISL28022, seq, 0x0A)
        is_fault = bool((self._active_faults >> FAULT_BIT_I2C_ISL28022) & 1)
        status = 1 if is_fault else 0
        cfg_hi = (self._isl_config_reg >> 8) & 0xFF
        cfg_lo = self._isl_config_reg & 0xFF
        if is_fault:
            cfg_hi = cfg_lo = 0xFF
        return self._respond(PKT_PROBE_ISL28022, bytes([status, cfg_hi, cfg_lo]), seq)

    def _h_read_vpack_raw(self, seq: int) -> bytes:
        if self._firmware_type != FIRMWARE_TYPE_BMS_APP:
            return self._error(PKT_READ_VPACK_RAW, seq, 0x0A)
        is_fault = bool((self._active_faults >> FAULT_BIT_VPACK_INVALID) & 1)
        status = 1 if is_fault else 0
        raw = 0 if is_fault else self._vpack_raw
        return self._respond(PKT_READ_VPACK_RAW, struct.pack('<BH', status, raw & 0xFFFF), seq)

    def _h_balance_disable_all(self, seq: int) -> bytes:
        if self._firmware_type != FIRMWARE_TYPE_BMS_APP:
            return self._error(PKT_BALANCE_DISABLE_ALL, seq, 0x0A)
        return self._respond(PKT_BALANCE_DISABLE_ALL, bytes([0]), seq)

    def _h_measure_cells_once(self, seq: int) -> bytes:
        if self._firmware_type != FIRMWARE_TYPE_BMS_APP:
            return self._error(PKT_MEASURE_CELLS_ONCE, seq, 0x0A)
        is_fault = bool((self._active_faults >> FAULT_BIT_ISOSPI_CELL) & 1)
        status = 1 if is_fault else 0
        # status(1) + cell_count(2) + mv[75×2](150) + ts(4) + valid_bits(10) = 167
        resp = bytearray(167)
        resp[0] = status
        struct.pack_into('<H', resp, 1, TOTAL_CELL_COUNT)
        for i, mv in enumerate(self._cell_mv):
            struct.pack_into('<H', resp, 3 + i*2, 0 if is_fault else (mv & 0xFFFF))
        struct.pack_into('<I', resp, 153, self._uptime_ms)
        if not is_fault:
            for i in range(TOTAL_CELL_COUNT):
                resp[157 + i // 8] |= (1 << (i % 8))
        return self._respond(PKT_MEASURE_CELLS_ONCE, bytes(resp), seq)

    def _h_measure_temps_once(self, seq: int) -> bytes:
        if self._firmware_type != FIRMWARE_TYPE_BMS_APP:
            return self._error(PKT_MEASURE_TEMPS_ONCE, seq, 0x0A)
        is_fault = bool((self._active_faults >> FAULT_BIT_ISOSPI_TEMP) & 1)
        status = 1 if is_fault else 0
        # status(1) + temp_count(2) + cx10[75×2](150) + ts(4) = 157
        resp = bytearray(157)
        resp[0] = status
        struct.pack_into('<H', resp, 1, TOTAL_TEMP_COUNT)
        for i, t in enumerate(self._temps_cx10):
            val = TEMP_INVALID_CX10 if is_fault else t
            struct.pack_into('<H', resp, 3 + i*2, val & 0xFFFF)
        struct.pack_into('<I', resp, 153, self._uptime_ms)
        return self._respond(PKT_MEASURE_TEMPS_ONCE, bytes(resp), seq)

    def _h_measure_power_once(self, seq: int) -> bytes:
        if self._firmware_type != FIRMWARE_TYPE_BMS_APP:
            return self._error(PKT_MEASURE_POWER_ONCE, seq, 0x0A)
        i2c_fault   = bool((self._active_faults >> FAULT_BIT_I2C_ISL28022) & 1)
        vpack_fault = bool((self._active_faults >> FAULT_BIT_VPACK_INVALID) & 1)
        vbat_valid   = not i2c_fault
        vpack_valid  = not vpack_fault
        ibatt_valid  = not i2c_fault
        status = 1 if (i2c_fault or vpack_fault) else 0
        vbat_mv   = 48000 if vbat_valid else 0
        vpack_mv  = 48000 if vpack_valid else 0
        i_batt_ma = 0
        flags = ((1 if vbat_valid else 0) |
                 (2 if vpack_valid else 0) |
                 (4 if ibatt_valid else 0))
        # status(1) + vbat_mv(4) + vpack_mv(4) + i_batt_ma(4) + flags(1) + ts(4) = 18
        resp = bytearray(18)
        resp[0] = status
        struct.pack_into('<i', resp, 1,  vbat_mv)
        struct.pack_into('<i', resp, 5,  vpack_mv)
        struct.pack_into('<i', resp, 9,  i_batt_ma)
        resp[13] = flags
        struct.pack_into('<I', resp, 14, self._uptime_ms)
        return self._respond(PKT_MEASURE_POWER_ONCE, bytes(resp), seq)

    # ── State injection helpers ───────────────────────────────────────────────

    def inject_fault(self, fault_bit: int) -> None:
        self._active_faults  |= (1 << fault_bit)
        self._latched_faults |= (1 << fault_bit)

    def clear_fault(self, fault_bit: int) -> None:
        self._active_faults &= ~(1 << fault_bit)

    def set_cell_mv(self, values: list) -> None:
        self._cell_mv = list(values)[:TOTAL_CELL_COUNT]

    def set_temps_cx10(self, values: list) -> None:
        self._temps_cx10 = [int(t) for t in list(values)[:TOTAL_TEMP_COUNT]]

    def set_uptime_ms(self, ms: int) -> None:
        self._uptime_ms = ms

    def set_pec_errors(self, cell: int = 0, temp: int = 0) -> None:
        self._pec_errors_cell = cell
        self._pec_errors_temp = temp

    def set_open_wire(self, valid: bool, detected: Optional[list] = None) -> None:
        self._open_wire_valid = valid
        if detected:
            self._open_wire_detected = list(detected)[:TOTAL_CELL_COUNT]

    def set_firmware_type(self, fw_type: int) -> None:
        self._firmware_type = fw_type

    def set_soc(self, soc_pct_x10: int) -> None:
        self._soc_pct_x10 = max(-1, min(1000, soc_pct_x10))


# ── In-process helper for unit tests ─────────────────────────────────────────

class FakeTargetInProcess:
    """Wrap FakeTarget for synchronous in-process testing.

    Usage:
        ft = FakeTargetInProcess(mode='healthy')
        response_bytes = ft.exchange(request_bytes)
    """

    def __init__(self, mode: str = 'healthy'):
        self.target = FakeTarget(mode=mode)

    def exchange(self, data: bytes) -> bytes:
        """Feed data to the target and return the response synchronously."""
        return self.target.feed(data)

    def inject_fault(self, fault_bit: int) -> None:
        self.target.inject_fault(fault_bit)

    def clear_fault(self, fault_bit: int) -> None:
        self.target.clear_fault(fault_bit)

    def set_cell_mv(self, values: list) -> None:
        self.target.set_cell_mv(values)

    def set_temps_cx10(self, values: list) -> None:
        self.target.set_temps_cx10(values)


# ── Subprocess entry point ────────────────────────────────────────────────────

def run_on_serial_port(port_name: str, baud: int = 115200,
                       mode: str = 'healthy') -> None:
    """Run the fake target on a real or virtual serial port."""
    import serial
    target = FakeTarget(mode=mode)
    with serial.Serial(port_name, baud, timeout=0.1) as ser:
        print(f"[fake_target] listening on {port_name} at {baud} (mode={mode})")
        while True:
            data = ser.read(512)
            if data:
                resp = target.feed(data)
                if resp:
                    ser.write(resp)


if __name__ == '__main__':
    import argparse
    parser = argparse.ArgumentParser(description='BMS fake target')
    parser.add_argument('--port', required=True, help='Serial port (e.g. /dev/pts/3)')
    parser.add_argument('--baud', type=int, default=115200)
    parser.add_argument('--mode', default='healthy', choices=sorted(_KNOWN_MODES))
    parser.add_argument('--tcp', action='store_true',
                        help='Listen on TCP instead of serial (ignores --port/--baud)')
    parser.add_argument('--bind', default='127.0.0.1:65102',
                        metavar='HOST:PORT', help='TCP bind address (with --tcp)')
    args = parser.parse_args()

    if args.tcp:
        host, port_str = args.bind.rsplit(':', 1)
        FakeTarget.serve_tcp(host, int(port_str), mode=args.mode)
    else:
        run_on_serial_port(args.port, args.baud, args.mode)
