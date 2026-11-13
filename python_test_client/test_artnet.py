"""Unit tests for the Art-Net packet builders and parsers in artnet.py.

Run from this directory:  python3 -m unittest -v
No network or hardware is needed.
"""

import struct
import unittest

import artnet


def make_poll_reply(**over) -> bytearray:
    """A reply laid out like the one the ESP node sends (239 bytes)."""
    p = bytearray(239)
    p[0:8] = b'Art-Net\x00'
    p[8:10] = struct.pack('<H', 0x2100)
    p[10:14] = bytes([192, 168, 1, 50])
    p[14:16] = struct.pack('<H', 6454)
    p[18] = over.get('net', 0)
    p[19] = over.get('sub', 0)
    p[23] = over.get('status1', 0xE0)
    p[26:26 + 11] = b'ESP Art-Net'
    p[44:44 + 9] = b'Long name'
    p[108:108 + 15] = b'#0001 [0001] OK'
    p[173] = over.get('ports', 1)
    p[174] = over.get('port_type', 0x80)
    p[182] = over.get('good_output', 0x80)
    p[190] = over.get('uni', 0)
    p[201:207] = bytes([1, 2, 3, 4, 5, 6])
    p[212] = over.get('status2', 0x0F)
    p[217] = over.get('status3', 0x20)
    return p


class ArtDmxTest(unittest.TestCase):
    def test_sequence_never_zero(self):
        sender = artnet.ArtNetSender('127.0.0.1')
        try:
            sender._cached_ip = '127.0.0.1'
            seen = set()
            for _ in range(600):
                sender.send(0, bytes(4))
                seen.add(sender._seq)
            self.assertNotIn(0, seen)           # 0 would disable the sequence check
            self.assertEqual(seen, set(range(1, 256)))
        finally:
            sender.close()


class ArtPollTest(unittest.TestCase):
    def test_poll_packet(self):
        p = artnet.build_poll()
        self.assertEqual(len(p), 14)
        self.assertEqual(p[:8], b'Art-Net\x00')
        self.assertEqual(p[8:10], b'\x00\x20')      # opcode 0x2000, little-endian
        self.assertEqual(p[10:12], b'\x00\x0e')     # protocol 14, big-endian
        self.assertEqual(p[12], 0)

    def test_poll_flags(self):
        self.assertEqual(artnet.build_poll(artnet.POLL_TALK_ON_CHANGE)[12], 0x02)

    def test_parse_reply(self):
        n = artnet.parse_poll_reply(bytes(make_poll_reply(net=1, sub=2, uni=3)),
                                    ('192.168.1.50', 6454))
        self.assertEqual(n['ip'], '192.168.1.50')
        self.assertEqual(n['name'], 'ESP Art-Net')
        self.assertEqual(n['long_name'], 'Long name')
        self.assertEqual((n['net'], n['sub'], n['uni']), (1, 2, 3))
        self.assertEqual(n['universe'], 0x123)
        self.assertEqual(n['mac'], '01:02:03:04:05:06')
        self.assertEqual(n['indicator'], 'normal')
        self.assertTrue(n['dhcp'])
        self.assertEqual(n['style'], 'Node')

    def test_parse_reply_status(self):
        n = artnet.parse_poll_reply(
            bytes(make_poll_reply(status1=0x60, good_output=0x8A, status3=0x60,
                                  status2=0x0D)), ('1.2.3.4', 6454))
        self.assertEqual(n['indicator'], 'locate')
        self.assertTrue(n['active'])
        self.assertTrue(n['merging'])
        self.assertEqual(n['merge'], 'LTP')
        self.assertEqual(n['failsafe'], 'zero')
        self.assertFalse(n['dhcp'])

    def test_parse_reply_without_output_port(self):
        n = artnet.parse_poll_reply(bytes(make_poll_reply(port_type=0x00)),
                                    ('1.2.3.4', 6454))
        self.assertIsNone(n['universe'])

    def test_parse_reply_ip_fallback(self):
        p = make_poll_reply()
        p[10:14] = bytes(4)
        self.assertEqual(artnet.parse_poll_reply(bytes(p), ('10.0.0.9', 6454))['ip'],
                         '10.0.0.9')

    def test_parse_rejects_garbage(self):
        self.assertIsNone(artnet.parse_poll_reply(b'x' * 239, ('1.2.3.4', 1)))
        self.assertIsNone(artnet.parse_poll_reply(bytes(make_poll_reply())[:100],
                                                  ('1.2.3.4', 1)))
        wrong = make_poll_reply()
        wrong[8:10] = struct.pack('<H', 0x5000)
        self.assertIsNone(artnet.parse_poll_reply(bytes(wrong), ('1.2.3.4', 1)))


class ArtAddressTest(unittest.TestCase):
    def test_no_change_packet(self):
        p = artnet.build_art_address()
        self.assertEqual(len(p), 107)
        self.assertEqual(p[8:10], b'\x00\x60')          # opcode 0x6000
        self.assertEqual(p[12], 0x7F)                   # net: no change
        self.assertEqual(p[100], 0x7F)                  # universe: no change
        self.assertEqual(p[104], 0x7F)                  # sub-net: no change
        self.assertEqual(p[14], 0)                      # empty names
        self.assertEqual(p[32], 0)
        self.assertEqual(p[106], artnet.AC_NONE)

    def test_program_address(self):
        p = artnet.build_art_address(net=1, sub=2, universe=3)
        self.assertEqual(p[12], 0x81)
        self.assertEqual(p[104], 0x82)
        self.assertEqual(p[100], 0x83)
        self.assertEqual(p[101:104], b'\x7f\x7f\x7f')   # other ports untouched

    def test_zero_is_programmed_not_skipped(self):
        p = artnet.build_art_address(net=0, sub=0, universe=0)
        self.assertEqual((p[12], p[104], p[100]), (0x80, 0x80, 0x80))

    def test_names_are_truncated_and_terminated(self):
        p = artnet.build_art_address(short_name='S' * 40, long_name='L' * 100)
        self.assertEqual(p[14:31], b'S' * 17)
        self.assertEqual(p[31], 0)
        self.assertEqual(p[32:95], b'L' * 63)
        self.assertEqual(p[95], 0)

    def test_command(self):
        self.assertEqual(artnet.build_art_address(command=artnet.AC_LED_LOCATE)[106], 0x04)
        self.assertEqual(artnet.build_art_address(command=artnet.AC_FAIL_ZERO)[106], 0x09)


class ArtIpProgTest(unittest.TestCase):
    def test_query(self):
        p = artnet.build_ip_prog(program=False)
        self.assertEqual(len(p), 34)
        self.assertEqual(p[8:10], b'\x00\xf8')          # opcode 0xF800
        self.assertEqual(p[14], 0)

    def test_dhcp(self):
        self.assertEqual(artnet.build_ip_prog(dhcp=True)[14], 0xC0)

    def test_static(self):
        p = artnet.build_ip_prog(ip='192.168.1.5', mask='255.255.255.0',
                                 gateway='192.168.1.1')
        self.assertEqual(p[14], 0x80 | 0x08 | 0x04 | 0x20)
        self.assertEqual(p[16:20], bytes([192, 168, 1, 5]))
        self.assertEqual(p[20:24], bytes([255, 255, 255, 0]))
        self.assertEqual(p[26:30], bytes([192, 168, 1, 1]))

    def test_partial(self):
        self.assertEqual(artnet.build_ip_prog(gateway='10.0.0.1')[14], 0x80 | 0x20)

    def test_invalid_address(self):
        with self.assertRaises(OSError):
            artnet.build_ip_prog(ip='999.1.1.1')

    def test_parse_reply(self):
        r = bytearray(34)
        r[0:8] = b'Art-Net\x00'
        r[8:10] = struct.pack('<H', 0xF900)
        r[16:20] = bytes([10, 0, 0, 5])
        r[20:24] = bytes([255, 255, 0, 0])
        r[26] = 0x40
        r[28:32] = bytes([10, 0, 0, 1])
        d = artnet.parse_ip_prog_reply(bytes(r), ('10.0.0.5', 6454))
        self.assertEqual(d, {'from': '10.0.0.5', 'ip': '10.0.0.5',
                             'mask': '255.255.0.0', 'gateway': '10.0.0.1',
                             'dhcp': True})
        self.assertIsNone(artnet.parse_ip_prog_reply(bytes(r)[:20], ('a', 1)))
        self.assertIsNone(artnet.parse_ip_prog_reply(bytes(make_poll_reply()), ('a', 1)))


if __name__ == '__main__':
    unittest.main()
