"""Art-Net ArtDMX (opcode 0x5000) UDP sender."""

import select
import socket
import struct
import threading
import time

ARTNET_PORT = 6454
_HEADER     = b'Art-Net\x00'
_OP_DMX     = 0x5000
_PROT_VER   = 14
_OP_POLL       = 0x2000
_OP_POLL_REPLY = 0x2100
_OP_ADDRESS    = 0x6000
_OP_IP_PROG    = 0xF800
_OP_IP_PROG_REPLY = 0xF900

# ArtIpProg command bits
IPPROG_ENABLE, IPPROG_DHCP, IPPROG_GATEWAY = 0x80, 0x40, 0x20
IPPROG_DEFAULTS, IPPROG_IP, IPPROG_MASK = 0x10, 0x08, 0x04

# ArtPoll TalkToMe flag: node should send ArtPollReply whenever it changes
POLL_TALK_ON_CHANGE = 0x02

# ArtAddress commands (subset)
AC_NONE, AC_LED_NORMAL, AC_LED_MUTE, AC_LED_LOCATE = 0x00, 0x02, 0x03, 0x04
AC_CLEAR_OP0 = 0x90

_INDICATORS = {0: "unknown", 1: "locate", 2: "mute", 3: "normal"}

_STYLES = {0: "Node", 1: "Controller", 2: "Media server", 3: "Route",
           4: "Backup", 5: "Config", 6: "Visual"}


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


# ── Discovery (ArtPoll / ArtPollReply) ────────────────────────────────────────

def build_poll(flags: int = 0) -> bytes:
    """ArtPoll packet. flags = TalkToMe byte (0 = reply only to polls)."""
    return (_HEADER + struct.pack('<H', _OP_POLL)
            + struct.pack('>H', _PROT_VER) + bytes([flags & 0xFF, 0]))


def build_art_address(net: int | None = None, sub: int | None = None,
                      universe: int | None = None,
                      short_name: str = "", long_name: str = "",
                      command: int = AC_NONE) -> bytes:
    """ArtAddress packet (107 bytes) to reprogram a node's output port 1.

    net (0-127), sub (0-15) and universe (0-15) are the three parts of the
    15-bit port-address; None leaves the respective part unchanged. Empty
    names are not changed either."""
    def prog(value, mask):
        return 0x7F if value is None else (0x80 | (value & mask))

    pkt = bytearray(107)
    pkt[0:8] = _HEADER
    pkt[8:10] = struct.pack('<H', _OP_ADDRESS)
    pkt[10:12] = struct.pack('>H', _PROT_VER)
    pkt[12] = prog(net, 0x7F)                       # NetSwitch
    pkt[13] = 1                                     # BindIndex
    pkt[14:32] = short_name.encode('ascii', 'replace')[:17].ljust(18, b'\x00')
    pkt[32:96] = long_name.encode('ascii', 'replace')[:63].ljust(64, b'\x00')
    pkt[96:100] = bytes([0x7F] * 4)                 # SwIn: no change
    pkt[100:104] = bytes([prog(universe, 0x0F), 0x7F, 0x7F, 0x7F])   # SwOut
    pkt[104] = prog(sub, 0x0F)                      # SubSwitch
    pkt[105] = 0x00                                 # SwVideo
    pkt[106] = command & 0xFF                       # Command
    return bytes(pkt)


def build_ip_prog(ip: str | None = None, mask: str | None = None,
                  gateway: str | None = None, dhcp: bool = False,
                  program: bool = True) -> bytes:
    """ArtIpProg packet (34 bytes) for remote IP configuration.

    program=False sends command 0, which only queries the current settings.
    dhcp=True switches the node to DHCP. Otherwise every given address is
    programmed (None leaves that part unchanged)."""
    cmd = 0
    if program:
        cmd = IPPROG_ENABLE
        if dhcp:
            cmd |= IPPROG_DHCP
        else:
            if ip:      cmd |= IPPROG_IP
            if mask:    cmd |= IPPROG_MASK
            if gateway: cmd |= IPPROG_GATEWAY
    pkt = bytearray(34)
    pkt[0:8] = _HEADER
    pkt[8:10] = struct.pack('<H', _OP_IP_PROG)
    pkt[10:12] = struct.pack('>H', _PROT_VER)
    pkt[14] = cmd
    for off, text in ((16, ip), (20, mask), (26, gateway)):
        if text:
            pkt[off:off + 4] = socket.inet_aton(text)   # raises OSError if invalid
    return bytes(pkt)


def parse_ip_prog_reply(data: bytes, addr: tuple) -> dict | None:
    """Decode an ArtIpProgReply; returns None if data is not one."""
    if len(data) < 34 or data[:8] != _HEADER:
        return None
    if struct.unpack('<H', data[8:10])[0] != _OP_IP_PROG_REPLY:
        return None
    return {
        "from":    addr[0],
        "ip":      socket.inet_ntoa(data[16:20]),
        "mask":    socket.inet_ntoa(data[20:24]),
        "gateway": socket.inet_ntoa(data[28:32]),
        "dhcp":    bool(data[26] & 0x40),
    }


def parse_poll_reply(data: bytes, addr: tuple) -> dict | None:
    """Decode an ArtPollReply; returns None if data is not a valid reply."""
    if len(data) < 213 or data[:8] != _HEADER:
        return None
    if struct.unpack('<H', data[8:10])[0] != _OP_POLL_REPLY:
        return None

    def text(a: int, b: int) -> str:
        return data[a:b].split(b'\x00', 1)[0].decode('ascii', 'replace')

    ip = '.'.join(str(b) for b in data[10:14])
    if ip == '0.0.0.0':
        ip = addr[0]

    # Port-address of the first DMX output port (15 bit: net / sub-net / universe)
    universe = None
    num_ports = min(struct.unpack('>H', data[172:174])[0], 4)
    for i in range(num_ports):
        if data[174 + i] & 0x80:                       # port can output DMX
            universe = ((data[18] & 0x7F) << 8) | ((data[19] & 0x0F) << 4) \
                       | (data[190 + i] & 0x0F)
            break

    net, sub = data[18] & 0x7F, data[19] & 0x0F
    return {
        "net":       net,
        "sub":       sub,
        "uni":       (data[190] & 0x0F) if universe is not None else None,
        "indicator": _INDICATORS[data[23] >> 6],
        "dhcp":      bool(data[212] & 0x02),
        "ip":        ip,
        "name":      text(26, 44),
        "long_name": text(44, 108),
        "report":    text(108, 172),
        "universe":  universe,
        "style":     _STYLES.get(data[200], f"Style {data[200]}"),
        "mac":       ':'.join(f"{b:02x}" for b in data[201:207]),
    }


def _broadcast_targets() -> list[str]:
    """Limited broadcast plus the directed broadcast of the default interface
    (assuming a /24 subnet, which covers typical home / stage networks)."""
    targets = ['255.255.255.255']
    try:
        probe = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        probe.connect(('192.0.2.1', 9))      # no packet is sent for UDP connect
        local = probe.getsockname()[0]
        probe.close()
        directed = '.'.join(local.split('.')[:3] + ['255'])
        if directed not in targets:
            targets.append(directed)
    except OSError:
        pass
    return targets


def _udp_socket(port: int, optional: bool = False):
    """Broadcast-capable UDP socket bound to `port`. Port 6454 is shared with
    other Art-Net programs (SO_REUSEADDR / SO_REUSEPORT); if it cannot be bound
    and optional is set, None is returned."""
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    if hasattr(socket, 'SO_REUSEPORT'):
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1)
    try:
        sock.bind(('', port))
    except OSError:
        sock.close()
        if optional:
            return None
        raise
    return sock


def scan_nodes(timeout: float = 2.0, on_found=None,
               stop: threading.Event | None = None) -> list[dict]:
    """Broadcast ArtPoll and collect ArtPollReply packets for `timeout` s.

    on_found(node_dict) is called (from the calling thread) for every new
    device; `stop` aborts the scan early. Returns the list of devices found.
    """
    # Poll from an ephemeral port: this node (like most) replies to the poll's
    # source address, so its answer cannot be taken by another program that
    # shares port 6454. A second socket bound to 6454 (when possible) also
    # catches replies that other nodes broadcast to that port.
    tx = _udp_socket(0)
    rx = _udp_socket(ARTNET_PORT, optional=True)
    socks = [tx] + ([rx] if rx else [])

    found: dict[str, dict] = {}
    poll = build_poll()
    deadline = time.monotonic() + timeout
    next_poll = 0.0
    try:
        while time.monotonic() < deadline and not (stop and stop.is_set()):
            now = time.monotonic()
            if now >= next_poll:             # poll twice to survive packet loss
                for target in _broadcast_targets():
                    try:
                        tx.sendto(poll, (target, ARTNET_PORT))
                    except OSError:
                        pass
                next_poll = now + 1.0
            try:
                ready, _, _ = select.select(socks, [], [], 0.1)
            except (OSError, ValueError):
                break
            for sock in ready:
                try:
                    data, addr = sock.recvfrom(1024)
                except OSError:
                    continue
                node = parse_poll_reply(data, addr)
                if node and node["ip"] not in found:
                    found[node["ip"]] = node
                    if on_found:
                        on_found(node)
    finally:
        for sock in socks:
            sock.close()
    return list(found.values())


def query_node(host: str, timeout: float = 0.5) -> dict | None:
    """Unicast an ArtPoll to `host` and return its ArtPollReply (or None)."""
    try:
        ip = socket.gethostbyname(host)
    except OSError:
        return None
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        sock.settimeout(0.1)
        sock.sendto(build_poll(), (ip, ARTNET_PORT))
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            try:
                data, addr = sock.recvfrom(1024)
            except socket.timeout:
                continue
            node = parse_poll_reply(data, addr)
            if node and node["ip"] == ip:
                return node
    except OSError:
        pass
    finally:
        sock.close()
    return None


class NodeLink:
    """Talks to a node: sends ArtPoll / ArtAddress / ArtIpProg and delivers
    every ArtPollReply to on_reply(node, addr) and every ArtIpProgReply to
    on_ip_reply(dict), from a background thread.

    Packets are sent from an ephemeral port, so the node's unicast answers
    reach this link even if other programs share port 6454. A second socket
    bound to port 6454 receives broadcasts such as unsolicited replies;
    `bound` tells whether that worked."""

    def __init__(self, on_reply, on_ip_reply=None):
        self._on_reply = on_reply
        self._on_ip_reply = on_ip_reply
        self._running = True
        self._tx = _udp_socket(0)
        self._rx = _udp_socket(ARTNET_PORT, optional=True)
        self.bound = self._rx is not None
        self._socks = [self._tx] + ([self._rx] if self._rx else [])
        threading.Thread(target=self._loop, daemon=True).start()

    def _loop(self) -> None:
        while self._running:
            try:
                ready, _, _ = select.select(self._socks, [], [], 0.2)
            except (OSError, ValueError):
                break
            for sock in ready:
                try:
                    data, addr = sock.recvfrom(1024)
                except OSError:
                    continue
                node = parse_poll_reply(data, addr)
                if node:
                    self._on_reply(node, addr)
                    continue
                ipr = parse_ip_prog_reply(data, addr)
                if ipr and self._on_ip_reply:
                    self._on_ip_reply(ipr)

    def send(self, ip: str, packet: bytes) -> bool:
        try:
            self._tx.sendto(packet, (ip, ARTNET_PORT))
            return True
        except OSError:
            return False

    def close(self) -> None:
        self._running = False
        for sock in self._socks:
            try:
                sock.close()
            except OSError:
                pass
