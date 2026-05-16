"""connection_manager.py — transport layer: TCP socket or serial port."""
import select
import socket
from typing import Optional


class TcpPort:
    """socket wrapper presenting a serial.Serial-compatible interface."""

    def __init__(self, host: str, port: int, timeout: float = 5.0):
        self._sock = socket.create_connection((host, port), timeout=timeout)
        self._sock.settimeout(None)
        self._buf  = bytearray()

    def write(self, data: bytes) -> None:
        self._sock.sendall(data)

    def read(self, n: int) -> bytes:
        while len(self._buf) < n:
            chunk = self._sock.recv(4096)
            if not chunk:
                raise IOError("TCP connection closed by remote")
            self._buf.extend(chunk)
        data = bytes(self._buf[:n])
        del self._buf[:n]
        return data

    @property
    def in_waiting(self) -> int:
        r, _, _ = select.select([self._sock], [], [], 0)
        if r:
            try:
                chunk = self._sock.recv(4096)
                if chunk:
                    self._buf.extend(chunk)
            except (BlockingIOError, OSError):
                pass
        return len(self._buf)

    def close(self) -> None:
        try:
            self._sock.close()
        except Exception:
            pass


class ConnectionManager:
    """Owns the active transport connection; returns a port-compatible object."""

    def __init__(self):
        self._port: Optional[object] = None

    def connect_tcp(self, host: str = '127.0.0.1', port: int = 65102) -> TcpPort:
        self.disconnect()
        p = TcpPort(host, port)
        self._port = p
        return p

    def connect_serial(self, device: str, baud: int = 115200) -> object:
        import serial  # optional dependency
        self.disconnect()
        p = serial.Serial(device, baud, timeout=0.05)
        self._port = p
        return p

    def disconnect(self) -> None:
        if self._port is not None:
            try:
                self._port.close()
            except Exception:
                pass
            self._port = None

    @property
    def port(self):
        return self._port

    @property
    def is_connected(self) -> bool:
        return self._port is not None
