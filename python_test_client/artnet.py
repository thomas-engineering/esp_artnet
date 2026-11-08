"""Art-Net ArtDMX (opcode 0x5000) UDP sender."""

import socket
import struct
import time

ARTNET_PORT = 6454
_HEADER     = b'Art-Net\x00'
_OP_DMX     = 0x5000
_PROT_VER   = 14


class ArtNetSender:
    """Sends Art-Net ArtDMX packets over UDP."""

    def __init__(self, host: str, port: int = ARTNET_PORT):
        self.host = host
        self.port = port
        self._sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self._sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        self._seq          = 0
        self._cached_ip: str | None = None
        self._last_resolve = 0.0

    @property
    def resolved_ip(self) -> str | None:
        return self._cached_ip

    def _get_ip(self) -> str | None:
        now = time.monotonic()
        if self._cached_ip is None or now - self._last_resolve > 5.0:
            try:
                self._cached_ip  = socket.gethostbyname(self.host)
                self._last_resolve = now
            except OSError:
                self._cached_ip = None
        return self._cached_ip

    def send(self, universe: int, dmx_data: bytes | bytearray) -> bool:
        """Send a DMX frame. universe is a 15-bit Art-Net port address. Returns True on success."""
        ip = self._get_ip()
        if ip is None:
            return False

        data = bytearray(dmx_data)[:512]
        # Packet length must be even and >= 2
        if len(data) < 2:
            data += b'\x00' * (2 - len(data))
        if len(data) % 2:
            data += b'\x00'

        self._seq = (self._seq + 1) & 0xFF
        sub_uni = universe & 0xFF
        net     = (universe >> 8) & 0x7F

        packet = (
            _HEADER
            + struct.pack('<H', _OP_DMX)       # OpCode LE
            + struct.pack('>H', _PROT_VER)     # ProtVer BE
            + bytes([self._seq, 0])            # Sequence, Physical
            + bytes([sub_uni, net])            # SubUni, Net
            + struct.pack('>H', len(data))     # Length BE
            + bytes(data)
        )
        try:
            self._sock.sendto(packet, (ip, self.port))
            return True
        except OSError:
            return False

    def close(self) -> None:
        try:
            self._sock.close()
        except OSError:
            pass
