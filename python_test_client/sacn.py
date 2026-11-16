"""E1.31 (sACN, ANSI E1.31 / Streaming ACN) data packet builder, parser and UDP sender.

Used to test the node's sACN input. Packets go to UDP port 5568, either
unicast to the node or to the universe's multicast group 239.255.<hi>.<lo>.
"""

import socket
import struct
import threading
import uuid

SACN_PORT        = 5568
SACN_MAX_UNIVERSE = 63999
PRIORITY_DEFAULT = 100

_ACN_ID       = b'ASC-E1.17\x00\x00\x00'
_VECTOR_ROOT  = 0x00000004      # E131_DATA_PACKET
_VECTOR_FRAME = 0x00000002      # E131_DATA_PACKET (framing layer)
_VECTOR_DMP   = 0x02            # DMP_SET_PROPERTY
OPT_TERMINATED = 0x40           # Stream_Terminated
OPT_PREVIEW    = 0x80           # Preview_Data


def multicast_address(universe: int) -> str:
    """Multicast group of a universe: 239.255.<hi>.<lo>."""
    return f"239.255.{(universe >> 8) & 0xFF}.{universe & 0xFF}"


def new_cid() -> bytes:
    """A random 16-byte component identifier (one per sender)."""
    return uuid.uuid4().bytes


def build_data_packet(universe: int, dmx_data: bytes | bytearray, seq: int,
                      cid: bytes, source_name: str = "ESP test client",
                      priority: int = PRIORITY_DEFAULT,
                      options: int = 0) -> bytes:
    """E1.31 data packet for the NULL start code (0x00) and up to 512 channels.

    universe is 1..63999, seq 0..255 (wraps), priority 0..200."""
    if not 1 <= universe <= SACN_MAX_UNIVERSE:
        raise ValueError("sACN universe must be 1..63999")
    if not 0 <= priority <= 200:
        raise ValueError("sACN priority must be 0..200")
    if len(cid) != 16:
        raise ValueError("CID must be 16 bytes")
    data = bytes(dmx_data)[:512]
    count = len(data) + 1                               # start code + channels

    dmp = (struct.pack('>H', 0x7000 | (10 + count))     # flags + length
           + bytes([_VECTOR_DMP, 0xA1])                 # vector, address type
           + struct.pack('>HHH', 0, 1, count)           # first address, increment, count
           + b'\x00' + data)                            # start code + channel data
    framing_body = (struct.pack('>I', _VECTOR_FRAME)
                    + source_name.encode('utf-8', 'replace')[:63].ljust(64, b'\x00')
                    + bytes([priority])
                    + struct.pack('>H', 0)              # synchronization address
                    + bytes([seq & 0xFF, options & 0xFF])
                    + struct.pack('>H', universe)
                    + dmp)
    framing = struct.pack('>H', 0x7000 | (2 + len(framing_body))) + framing_body
    root_body = struct.pack('>I', _VECTOR_ROOT) + cid + framing
    root = struct.pack('>H', 0x7000 | (2 + len(root_body))) + root_body
    return struct.pack('>HH', 0x0010, 0x0000) + _ACN_ID + root


def parse_data_packet(packet: bytes) -> dict | None:
    """Parses an E1.31 data packet; None if it is not a valid one."""
    if len(packet) < 126 or packet[0:4] != b'\x00\x10\x00\x00' \
            or packet[4:16] != _ACN_ID:
        return None
    if struct.unpack_from('>I', packet, 18)[0] != _VECTOR_ROOT \
            or struct.unpack_from('>I', packet, 40)[0] != _VECTOR_FRAME \
            or packet[117] != _VECTOR_DMP:
        return None
    count = struct.unpack_from('>H', packet, 123)[0]
    if count < 1 or 125 + count > len(packet):
        return None
    return {
        'cid': packet[22:38],
        'source_name': packet[44:108].split(b'\x00', 1)[0].decode('utf-8', 'replace'),
        'priority': packet[108],
        'seq': packet[111],
        'options': packet[112],
        'universe': struct.unpack_from('>H', packet, 113)[0],
        'start_code': packet[125],
        'data': packet[126:125 + count],
    }


class SacnSender:
    """Sends E1.31 data packets over UDP, unicast to `host` or multicast."""

    def __init__(self, host: str, multicast: bool = False,
                 source_name: str = "ESP test client", priority: int = PRIORITY_DEFAULT):
        self.host = host
        self.multicast = multicast
        self.source_name = source_name
        self.priority = priority
        self.cid = new_cid()
        self._sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self._sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 4)
        self._seq = 0
        self._cached_ip: str | None = None
        self._closed = threading.Event()
        # Name resolution (mDNS can take seconds) runs in its own thread so
        # that it never stalls the frame stream.
        threading.Thread(target=self._resolve_loop, daemon=True).start()

    @property
    def resolved_ip(self) -> str | None:
        return self._cached_ip

    def _resolve_loop(self) -> None:
        while not self._closed.is_set():
            try:
                self._cached_ip = socket.gethostbyname(self.host)
            except OSError:
                pass                    # keep the last address that worked
            self._closed.wait(5.0)

    def send(self, universe: int, dmx_data: bytes | bytearray,
             options: int = 0) -> bool:
        """Send a DMX frame to sACN universe 1..63999. Returns True on success."""
        ip = self._cached_ip
        if ip is None and not self.multicast:
            return False
        try:
            packet = build_data_packet(universe, dmx_data, self._seq, self.cid,
                                       self.source_name, self.priority, options)
        except ValueError:
            return False
        self._seq = (self._seq + 1) & 0xFF
        dest = multicast_address(universe) if self.multicast else ip
        try:
            self._sock.sendto(packet, (dest, SACN_PORT))
            return True
        except OSError:
            return False

    def terminate(self, universe: int) -> None:
        """Tells receivers that this source stops (Stream_Terminated, sent 3 times)."""
        for _ in range(3):
            self.send(universe, bytes(2), OPT_TERMINATED)

    def close(self) -> None:
        self._closed.set()
        try:
            self._sock.close()
        except OSError:
            pass
