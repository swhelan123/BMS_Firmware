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
    ENTER_BOOTLOADER_MAGIC, CONFIG_SCHEMA_SIZE,
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
