from __future__ import annotations

import struct
import unittest

from livox_mid360_diagnostics.udp_monitor import PACKET_HEADER, parse_packet


def make_packet(data_type: int, dot_num: int = 7, frame: int = 3, udp_count: int = 9) -> bytes:
    header = PACKET_HEADER.pack(
        1,
        PACKET_HEADER.size,
        0,
        dot_num,
        udp_count,
        frame,
        data_type,
        0,
        b"\x00" * 12,
        0,
        b"\x00" * 8,
    )
    return header + (b"\x00" * 64)


class ParsePacketTest(unittest.TestCase):
    def test_parse_packet_reads_header_fields(self) -> None:
        self.assertEqual(parse_packet(make_packet(data_type=1)), (1, 7, 3, 9))

    def test_parse_packet_estimates_dot_num_when_header_zero(self) -> None:
        payload = PACKET_HEADER.pack(
            1,
            PACKET_HEADER.size,
            0,
            0,
            2,
            4,
            1,
            0,
            b"\x00" * 12,
            0,
            b"\x00" * 8,
        ) + (b"\x00" * (14 * 5))
        self.assertEqual(parse_packet(payload), (1, 5, 4, 2))

    def test_parse_packet_rejects_short_payload(self) -> None:
        self.assertEqual(parse_packet(struct.pack("<I", 1)), (None, None, None, None))


if __name__ == "__main__":
    unittest.main()
