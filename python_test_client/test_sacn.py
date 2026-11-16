"""Unit tests for the E1.31 (sACN) packet builder and sender in sacn.py.

Run from this directory:  python3 -m unittest -v
No network or hardware is needed (the sender test uses the loopback interface).
"""

import socket
import struct
import unittest

import sacn

CID = bytes(range(16))


class DataPacketTest(unittest.TestCase):
    def test_layout(self):
        p = sacn.build_data_packet(7, bytes([1, 2, 3, 4]), seq=9, cid=CID,
                                   source_name="src", priority=120)
        self.assertEqual(len(p), 126 + 4)
        self.assertEqual(p[0:4], b'\x00\x10\x00\x00')           # preamble, postamble
        self.assertEqual(p[4:16], b'ASC-E1.17\x00\x00\x00')
        self.assertEqual(p[16:18], struct.pack('>H', 0x7000 | (len(p) - 16)))
        self.assertEqual(p[18:22], b'\x00\x00\x00\x04')         # root vector
        self.assertEqual(p[22:38], CID)
        self.assertEqual(p[38:40], struct.pack('>H', 0x7000 | (len(p) - 38)))
        self.assertEqual(p[40:44], b'\x00\x00\x00\x02')         # framing vector
        self.assertEqual(p[44:47], b'src')
        self.assertEqual(p[47:108], bytes(61))                  # padded name
        self.assertEqual(p[108], 120)                           # priority
        self.assertEqual(p[109:111], b'\x00\x00')               # sync address
        self.assertEqual(p[111], 9)                             # sequence
        self.assertEqual(p[112], 0)                             # options
        self.assertEqual(p[113:115], b'\x00\x07')               # universe, big-endian
        self.assertEqual(p[115:117], struct.pack('>H', 0x7000 | (len(p) - 115)))
        self.assertEqual(p[117:119], b'\x02\xa1')               # DMP vector, address type
        self.assertEqual(p[119:123], b'\x00\x00\x00\x01')       # first address, increment
        self.assertEqual(p[123:125], b'\x00\x05')               # count = start code + 4
        self.assertEqual(p[125], 0)                             # NULL start code
        self.assertEqual(p[126:], bytes([1, 2, 3, 4]))

    def test_full_frame_length(self):
        p = sacn.build_data_packet(1, bytes(512), 0, CID)
        self.assertEqual(len(p), 638)
        self.assertEqual(p[16:18], b'\x72\x6e')                 # 0x7000 | 622
        self.assertEqual(p[123:125], b'\x02\x01')               # 513

    def test_data_is_capped_at_512(self):
        self.assertEqual(len(sacn.build_data_packet(1, bytes(600), 0, CID)), 638)

    def test_round_trip(self):
        p = sacn.build_data_packet(300, b'\x10\x20', 255, CID, "n", 5,
                                   sacn.OPT_TERMINATED)
        d = sacn.parse_data_packet(p)
        self.assertEqual(d['universe'], 300)
        self.assertEqual(d['seq'], 255)
        self.assertEqual(d['priority'], 5)
        self.assertEqual(d['cid'], CID)
        self.assertEqual(d['source_name'], 'n')
        self.assertEqual(d['options'], sacn.OPT_TERMINATED)
        self.assertEqual(d['data'], b'\x10\x20')

    def test_invalid_arguments(self):
        for universe in (0, 64000):
            with self.assertRaises(ValueError):
                sacn.build_data_packet(universe, b'\x00', 0, CID)
        with self.assertRaises(ValueError):
            sacn.build_data_packet(1, b'\x00', 0, CID, priority=201)
        with self.assertRaises(ValueError):
            sacn.build_data_packet(1, b'\x00', 0, b'short')

    def test_parse_rejects_garbage(self):
        self.assertIsNone(sacn.parse_data_packet(b'Art-Net\x00' + bytes(200)))
        self.assertIsNone(sacn.parse_data_packet(bytes(10)))

    def test_multicast_address(self):
        self.assertEqual(sacn.multicast_address(1), '239.255.0.1')
        self.assertEqual(sacn.multicast_address(0x1234), '239.255.18.52')
        self.assertEqual(sacn.multicast_address(63999), '239.255.249.255')


class SenderTest(unittest.TestCase):
    def test_unicast_send_and_sequence_wrap(self):
        rx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        rx.bind(('127.0.0.1', 0))
        rx.settimeout(1)
        sender = sacn.SacnSender('127.0.0.1')
        sender._cached_ip = '127.0.0.1'
        sender._sock.close()
        sender._sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            self.assertFalse(sender.send(0, bytes(4)))          # invalid universe
            # Redirect the port to the test receiver
            orig = sacn.SACN_PORT
            sacn.SACN_PORT = rx.getsockname()[1]
            try:
                sender._seq = 254
                seqs = []
                for _ in range(3):
                    self.assertTrue(sender.send(5, bytes([9, 8])))
                    d = sacn.parse_data_packet(rx.recvfrom(1000)[0])
                    seqs.append(d['seq'])
                    self.assertEqual(d['universe'], 5)
                    self.assertEqual(d['data'], bytes([9, 8]))
                    self.assertEqual(d['cid'], sender.cid)
                self.assertEqual(seqs, [254, 255, 0])
            finally:
                sacn.SACN_PORT = orig
        finally:
            sender.close()
            rx.close()

    def test_unresolved_host_does_not_send(self):
        sender = sacn.SacnSender('127.0.0.1')
        try:
            sender._cached_ip = None
            self.assertFalse(sender.send(1, bytes(2)))
        finally:
            sender.close()


if __name__ == '__main__':
    unittest.main()
