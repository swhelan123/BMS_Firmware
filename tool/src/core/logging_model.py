"""logging_model.py — packet and event logging."""
import time
from dataclasses import dataclass
from typing import List
import threading


@dataclass
class PacketLogEntry:
    timestamp:  float
    direction:  str    # 'TX' or 'RX'
    pkt_id:     int
    seq:        int
    payload:    bytes
    is_error:   bool = False


@dataclass
class EventLogEntry:
    timestamp: float
    message:   str


class PacketLog:
    def __init__(self, max_entries: int = 500):
        self._lock    = threading.Lock()
        self._entries: List[PacketLogEntry] = []
        self._max     = max_entries

    def log_tx(self, pkt_id: int, seq: int, payload: bytes) -> None:
        self._add(PacketLogEntry(time.time(), 'TX', pkt_id, seq, payload))

    def log_rx(self, pkt_id: int, seq: int, payload: bytes, is_error: bool = False) -> None:
        self._add(PacketLogEntry(time.time(), 'RX', pkt_id, seq, payload, is_error))

    def _add(self, e: PacketLogEntry) -> None:
        with self._lock:
            self._entries.append(e)
            if len(self._entries) > self._max:
                del self._entries[0]

    def entries(self) -> List[PacketLogEntry]:
        with self._lock:
            return list(self._entries)

    def clear(self) -> None:
        with self._lock:
            self._entries.clear()

    def export_text(self) -> str:
        lines = []
        for e in self.entries():
            ts  = time.strftime('%H:%M:%S', time.localtime(e.timestamp))
            err = ' [ERR]' if e.is_error else ''
            lines.append(
                f"[{ts}] {e.direction} PKT=0x{e.pkt_id:04X} seq={e.seq} "
                f"len={len(e.payload)}{err}  {e.payload.hex()}")
        return '\n'.join(lines)


class EventLog:
    def __init__(self, max_entries: int = 1000):
        self._lock    = threading.Lock()
        self._entries: List[EventLogEntry] = []
        self._max     = max_entries

    def append(self, message: str) -> None:
        with self._lock:
            self._entries.append(EventLogEntry(time.time(), message))
            if len(self._entries) > self._max:
                del self._entries[0]

    def entries(self) -> List[EventLogEntry]:
        with self._lock:
            return list(self._entries)

    def clear(self) -> None:
        with self._lock:
            self._entries.clear()

    def export_text(self) -> str:
        lines = []
        for e in self.entries():
            ts  = time.strftime('%H:%M:%S', time.localtime(e.timestamp))
            ms  = int(e.timestamp * 1000) % 1000
            lines.append(f"[{ts}.{ms:03d}] {e.message}")
        return '\n'.join(lines)
