"""client.py — BmsProtocolClient: blocking request/response with timeout and retry.

All methods raise ProtocolError subclasses on failure.
"""
import struct
import time
from typing import Optional

from .framing import encode_frame, FrameDecoder, FrameError
from .packet_defs import (
    PKT_GET_CAPABILITIES, PKT_GET_VALUES, PKT_GET_CELLS, PKT_GET_TEMPS,
    PKT_GET_FAULTS, PKT_CLEAR_LATCHED_FAULTS,
    PKT_GET_CONFIG, PKT_VALIDATE_CONFIG, PKT_SET_CONFIG_RAM, PKT_STORE_CONFIG,
    PKT_GET_BOOT_INFO, PKT_ENTER_BOOTLOADER,
    PKT_BOOT_UPDATE_BEGIN, PKT_BOOT_UPDATE_CHUNK,
    PKT_BOOT_UPDATE_FINALIZE, PKT_BOOT_UPDATE_ABORT,
    PKT_GET_GPIO_SNAPSHOT, PKT_GET_OUTPUTS_SNAPSHOT,
    PKT_PROBE_CELL_CHAIN, PKT_PROBE_TEMP_CHAIN,
    PKT_PROBE_ISL28022, PKT_READ_VPACK_RAW, PKT_BALANCE_DISABLE_ALL,
    PKT_MEASURE_CELLS_ONCE, PKT_MEASURE_TEMPS_ONCE, PKT_MEASURE_POWER_ONCE,
    ENTER_BOOTLOADER_MAGIC, CONFIG_SCHEMA_SIZE,
    TOTAL_CELL_COUNT, TOTAL_TEMP_COUNT,
)


class ProtocolError(Exception):
    pass

class TimeoutError(ProtocolError):
    pass

class CrcError(ProtocolError):
    pass

class ErrorResponse(ProtocolError):
    def __init__(self, pkt_id: int, error_code: int):
        self.pkt_id = pkt_id
        self.error_code = error_code
        super().__init__(f"PKT 0x{pkt_id:04X} → ERR 0x{error_code:02X}")


class BmsProtocolClient:
    DEFAULT_TIMEOUT = 2.0   # seconds
    DEFAULT_RETRIES = 2

    def __init__(self, port):
        """port: open serial.Serial or any object with read()/write() and in_waiting."""
        self._port = port
        self._decoder = FrameDecoder()
        self._seq = 0

    def _next_seq(self) -> int:
        s = self._seq
        self._seq = (self._seq + 1) & 0xFF
        return s

    def send_request(self, pkt_id: int, payload: bytes = b'',
                     timeout: float = DEFAULT_TIMEOUT,
                     retries: int = DEFAULT_RETRIES) -> bytes:
        """Send a request and wait for a matching response. Returns response payload."""
        seq = self._next_seq()
        frame = encode_frame(pkt_id, payload, seq=seq)

        for attempt in range(retries + 1):
            self._port.write(frame)
            deadline = time.monotonic() + timeout
            while time.monotonic() < deadline:
                waiting = getattr(self._port, 'in_waiting', 0)
                if waiting:
                    raw = self._port.read(waiting)
                    frames = self._decoder.feed(raw)
                    for f in frames:
                        if f['pkt_id'] == pkt_id and f['is_response']:
                            if f['is_error']:
                                code = f['payload'][0] if f['payload'] else 0
                                raise ErrorResponse(pkt_id, code)
                            return f['payload']
                time.sleep(0.005)
            if attempt < retries:
                self._decoder = FrameDecoder()  # reset decoder on retry
        raise TimeoutError(f"No response for PKT 0x{pkt_id:04X} after {retries+1} attempts")

    # ── Packet methods ───────────────────────────────────────────────────────

    def get_capabilities(self) -> dict:
        payload = self.send_request(PKT_GET_CAPABILITIES)
        if len(payload) < 26:
            raise ProtocolError("GET_CAPABILITIES response too short")
        return {
            'firmware_type':         struct.unpack_from('<H', payload, 0)[0],
            'firmware_version':      tuple(payload[2:5]),
            'hw_profile_id':         struct.unpack_from('<H', payload, 5)[0],
            'protocol_version':      struct.unpack_from('<H', payload, 7)[0],
            'config_schema_version': struct.unpack_from('<H', payload, 9)[0],
            'cell_count':            payload[11],
            'temp_count':            payload[12],
            'feature_flags':         struct.unpack_from('<I', payload, 13)[0],
            'max_payload_log2':      payload[17],
            'flash_app_size':        struct.unpack_from('<I', payload, 18)[0],
            'flash_config_size':     struct.unpack_from('<I', payload, 22)[0],
        }

    def get_values(self) -> dict:
        p = self.send_request(PKT_GET_VALUES)
        return {
            'vbat_mv':           struct.unpack_from('<i', p, 0)[0],
            'vpack_mv':          struct.unpack_from('<i', p, 4)[0],
            'i_batt_ma':         struct.unpack_from('<i', p, 8)[0],
            'state':             struct.unpack_from('<H', p, 12)[0],
            'active_faults':     struct.unpack_from('<Q', p, 14)[0],
            'latched_faults':    struct.unpack_from('<Q', p, 22)[0],
            'outputs_state':     p[30],
            'uptime_ms':         struct.unpack_from('<I', p, 31)[0],
            'measurement_flags': p[35],
        }

    def get_cells(self, include_validity: bool = False) -> dict:
        req = bytes([0x01 if include_validity else 0x00])
        p = self.send_request(PKT_GET_CELLS, req)
        cell_count = struct.unpack_from('<H', p, 0)[0]
        cells = [struct.unpack_from('<H', p, 2 + i*2)[0] for i in range(cell_count)]
        result = {'cell_count': cell_count, 'cells_mv': cells,
                  'timestamp_ms': struct.unpack_from('<I', p, 152)[0]}
        if include_validity and len(p) >= 166:
            validity = []
            for i in range(cell_count):
                validity.append(bool(p[156 + i//8] & (1 << (i % 8))))
            result['validity'] = validity
        return result

    def get_temps(self) -> dict:
        p = self.send_request(PKT_GET_TEMPS)
        count = struct.unpack_from('<H', p, 0)[0]
        temps = [struct.unpack_from('<h', p, 2 + i*2)[0] for i in range(count)]
        return {'temp_count': count, 'temps_cx10': temps}

    def get_faults(self) -> dict:
        p = self.send_request(PKT_GET_FAULTS)
        return {
            'active_faults':  struct.unpack_from('<Q', p, 0)[0],
            'latched_faults': struct.unpack_from('<Q', p, 8)[0],
        }

    def clear_latched_faults(self, mask: int) -> int:
        req = struct.pack('<Q', mask)
        p = self.send_request(PKT_CLEAR_LATCHED_FAULTS, req)
        return struct.unpack_from('<Q', p, 0)[0]  # mask of cleared faults

    def get_config(self) -> bytes:
        return self.send_request(PKT_GET_CONFIG)

    def validate_config(self, cfg: bytes) -> tuple:
        if len(cfg) != CONFIG_SCHEMA_SIZE:
            raise ProtocolError("Config blob must be 226 bytes")
        p = self.send_request(PKT_VALIDATE_CONFIG, cfg)
        ok = (p[0] == 0)
        err_off = struct.unpack_from('<H', p, 1)[0] if len(p) >= 3 else 0xFFFF
        return ok, err_off

    def set_config_ram(self, cfg: bytes) -> tuple:
        p = self.send_request(PKT_SET_CONFIG_RAM, cfg)
        ok = (p[0] == 0)
        err_off = struct.unpack_from('<H', p, 1)[0] if len(p) >= 3 else 0xFFFF
        return ok, err_off

    def store_config(self, cfg: bytes) -> bool:
        p = self.send_request(PKT_STORE_CONFIG, cfg)
        return p[0] == 0

    def enter_bootloader(self) -> None:
        req = struct.pack('<I', ENTER_BOOTLOADER_MAGIC)
        self.send_request(PKT_ENTER_BOOTLOADER, req)

    def get_boot_info(self) -> dict:
        p = self.send_request(PKT_GET_BOOT_INFO)
        return {'raw': p}  # full parse when bootloader protocol is finalised

    def boot_update_begin(self, header: bytes) -> dict:
        p = self.send_request(PKT_BOOT_UPDATE_BEGIN, header)
        return {
            'result':              p[0],
            'reject_reason':       p[1],
            'expected_chunk_size': struct.unpack_from('<I', p, 2)[0],
            'total_chunks':        struct.unpack_from('<I', p, 6)[0],
        }

    def boot_update_chunk(self, index: int, data: bytes) -> int:
        req = struct.pack('<II', index, len(data)) + data
        p = self.send_request(PKT_BOOT_UPDATE_CHUNK, req)
        return p[0]

    def boot_update_finalize(self) -> dict:
        p = self.send_request(PKT_BOOT_UPDATE_FINALIZE)
        return {'result': p[0], 'computed_crc': struct.unpack_from('<I', p, 1)[0]}

    def boot_update_abort(self) -> None:
        self.send_request(PKT_BOOT_UPDATE_ABORT)

    # ── Bring-up / bench diagnostic methods ─────────────────────────────────

    def get_gpio_snapshot(self) -> dict:
        p = self.send_request(PKT_GET_GPIO_SNAPSHOT)
        if len(p) < 9:
            raise ProtocolError(f"GET_GPIO_SNAPSHOT too short: {len(p)}")
        return {
            'cs_cell':            p[0],
            'cs_temp':            p[1],
            'power_button':       p[2],
            'charge_detect':      p[3],
            'power_enable':       p[4],
            'master_ok_raw':      p[5],
            'discharge_raw':      p[6],
            'charge_raw':         p[7],
            'charger_safety_raw': p[8],
        }

    def get_outputs_snapshot(self) -> dict:
        p = self.send_request(PKT_GET_OUTPUTS_SNAPSHOT)
        if len(p) < 2:
            raise ProtocolError(f"GET_OUTPUTS_SNAPSHOT too short: {len(p)}")
        return {'logical_state': p[0], 'raw_state': p[1]}

    def probe_cell_chain(self) -> dict:
        p = self.send_request(PKT_PROBE_CELL_CHAIN)
        return self._parse_chain_probe(p)

    def probe_temp_chain(self) -> dict:
        p = self.send_request(PKT_PROBE_TEMP_CHAIN)
        return self._parse_chain_probe(p)

    @staticmethod
    def _parse_chain_probe(p: bytes) -> dict:
        if len(p) < 2:
            raise ProtocolError(f"chain probe response too short: {len(p)}")
        status   = p[0]
        ic_count = p[1]
        ics = []
        for i in range(ic_count):
            off = 2 + i * 7
            if off + 7 > len(p):
                break
            ics.append({
                'responded': bool(p[off]),
                'cfga':      p[off+1:off+7].hex(),
            })
        return {'status': status, 'ic_count': ic_count, 'ics': ics}

    def probe_isl28022(self) -> dict:
        p = self.send_request(PKT_PROBE_ISL28022)
        if len(p) < 3:
            raise ProtocolError(f"PROBE_ISL28022 too short: {len(p)}")
        config_reg = (p[1] << 8) | p[2]
        return {'status': p[0], 'config_reg': config_reg}

    def read_vpack_raw(self) -> dict:
        p = self.send_request(PKT_READ_VPACK_RAW)
        if len(p) < 3:
            raise ProtocolError(f"READ_VPACK_RAW too short: {len(p)}")
        return {'status': p[0], 'raw_code': struct.unpack_from('<H', p, 1)[0]}

    def balance_disable_all(self) -> bool:
        p = self.send_request(PKT_BALANCE_DISABLE_ALL)
        return p[0] == 0

    # ── One-shot measurement methods ─────────────────────────────────────────

    def measure_cells_once(self) -> dict:
        """Trigger a cell measurement cycle and return the resulting snapshot.

        Response: status(1) + cell_count(2) + mv[75×2](150) + ts(4) + valid_bits(10).
        """
        p = self.send_request(PKT_MEASURE_CELLS_ONCE)
        if len(p) < 167:
            raise ProtocolError(f"MEASURE_CELLS_ONCE too short: {len(p)}")
        status     = p[0]
        cell_count = struct.unpack_from('<H', p, 1)[0]
        cells_mv   = [struct.unpack_from('<H', p, 3 + i*2)[0] for i in range(cell_count)]
        ts         = struct.unpack_from('<I', p, 153)[0]
        validity   = [bool(p[157 + i//8] & (1 << (i % 8))) for i in range(cell_count)]
        return {
            'status':     status,
            'cell_count': cell_count,
            'cells_mv':   cells_mv,
            'validity':   validity,
            'timestamp_ms': ts,
        }

    def measure_temps_once(self) -> dict:
        """Trigger a temperature measurement cycle and return the resulting snapshot.

        Response: status(1) + temp_count(2) + cx10[75×2](150) + ts(4).
        """
        p = self.send_request(PKT_MEASURE_TEMPS_ONCE)
        if len(p) < 157:
            raise ProtocolError(f"MEASURE_TEMPS_ONCE too short: {len(p)}")
        status     = p[0]
        temp_count = struct.unpack_from('<H', p, 1)[0]
        temps_cx10 = [struct.unpack_from('<h', p, 3 + i*2)[0] for i in range(temp_count)]
        ts         = struct.unpack_from('<I', p, 153)[0]
        return {
            'status':     status,
            'temp_count': temp_count,
            'temps_cx10': temps_cx10,
            'timestamp_ms': ts,
        }

    def measure_power_once(self) -> dict:
        """Trigger a pack measurement cycle (ISL28022 + Vpack ADC) and return results.

        Response: status(1) + vbat_mv(4) + vpack_mv(4) + i_batt_ma(4) + flags(1) + ts(4).
        flags: bit0=vbat_valid, bit1=vpack_valid, bit2=i_batt_valid.
        """
        p = self.send_request(PKT_MEASURE_POWER_ONCE)
        if len(p) < 18:
            raise ProtocolError(f"MEASURE_POWER_ONCE too short: {len(p)}")
        flags = p[13]
        return {
            'status':       p[0],
            'vbat_mv':      struct.unpack_from('<i', p, 1)[0],
            'vpack_mv':     struct.unpack_from('<i', p, 5)[0],
            'i_batt_ma':    struct.unpack_from('<i', p, 9)[0],
            'flags':        flags,
            'vbat_valid':   bool(flags & 1),
            'vpack_valid':  bool(flags & 2),
            'i_batt_valid': bool(flags & 4),
            'timestamp_ms': struct.unpack_from('<I', p, 14)[0],
        }
