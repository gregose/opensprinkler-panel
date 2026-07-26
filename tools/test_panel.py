#!/usr/bin/env python3
"""Unit tests for tools/panel.py screenshot decoding + PNG encoding.

Pure host-side logic — exercises the wire-protocol parser against a synthetic
byte stream (no device, no socket) so the reassembly/conversion path is covered
in CI. Run: python3 tools/test_panel.py
"""

import io
import os
import struct
import sys
import unittest
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import panel  # noqa: E402


def rgb565_le(r5, g6, b5):
    v = ((r5 & 0x1F) << 11) | ((g6 & 0x3F) << 5) | (b5 & 0x1F)
    return struct.pack("<H", v)


def build_stream(width, height, fill_le, *, leading_logs=b"", band=None):
    """Construct a full SHOT/STRIP*/END byte stream for a solid-color frame."""
    band = band or height  # rows per strip
    out = bytearray(leading_logs)
    out += b"\x02SHOT %d %d 565\n" % (width, height)
    y = 0
    while y < height:
        sh = min(band, height - y)
        out += b"\x02STRIP 0 %d %d %d\n" % (y, width, sh)
        out += fill_le * (width * sh)
        y += sh
    out += b"\x02END\n"
    return bytes(out)


class RgbConversionTests(unittest.TestCase):
    def test_pure_colors(self):
        # Max red 565 -> (255,0,0); max green -> (0,255,0); max blue -> (0,0,255).
        red = panel.rgb565_to_rgb888(rgb565_le(0x1F, 0, 0), 1)
        self.assertEqual(tuple(red), (255, 0, 0))
        green = panel.rgb565_to_rgb888(rgb565_le(0, 0x3F, 0), 1)
        self.assertEqual(tuple(green), (0, 255, 0))
        blue = panel.rgb565_to_rgb888(rgb565_le(0, 0, 0x1F), 1)
        self.assertEqual(tuple(blue), (0, 0, 255))

    def test_black_and_white(self):
        black = panel.rgb565_to_rgb888(rgb565_le(0, 0, 0), 1)
        self.assertEqual(tuple(black), (0, 0, 0))
        white = panel.rgb565_to_rgb888(rgb565_le(0x1F, 0x3F, 0x1F), 1)
        self.assertEqual(tuple(white), (255, 255, 255))


class ScreenshotParseTests(unittest.TestCase):
    def test_single_strip(self):
        stream = build_stream(4, 2, rgb565_le(0x1F, 0, 0))
        w, h, rgb = panel.read_screenshot(io.BytesIO(stream))
        self.assertEqual((w, h), (4, 2))
        self.assertEqual(len(rgb), 4 * 2 * 3)
        self.assertEqual(tuple(rgb[0:3]), (255, 0, 0))
        self.assertEqual(tuple(rgb[-3:]), (255, 0, 0))

    def test_multi_strip_reassembly(self):
        # 480x320 in 40-row bands, like the firmware's DRAW_BUF_LINES.
        stream = build_stream(480, 320, rgb565_le(0, 0, 0x1F), band=40)
        w, h, rgb = panel.read_screenshot(io.BytesIO(stream))
        self.assertEqual((w, h), (480, 320))
        self.assertEqual(len(rgb), 480 * 320 * 3)
        self.assertEqual(tuple(rgb[0:3]), (0, 0, 255))
        # Last pixel of the last band must be present (full coverage).
        self.assertEqual(tuple(rgb[-3:]), (0, 0, 255))

    def test_leading_logs_are_discarded(self):
        logs = b"[HB] beat=1 heap=100000\n[LOG] OSPanel log stream\n"
        stream = build_stream(2, 1, rgb565_le(0x1F, 0x3F, 0x1F), leading_logs=logs)
        w, h, rgb = panel.read_screenshot(io.BytesIO(stream))
        self.assertEqual((w, h), (2, 1))
        self.assertEqual(tuple(rgb[0:3]), (255, 255, 255))

    def test_truncated_stream_raises(self):
        stream = build_stream(4, 2, rgb565_le(0x1F, 0, 0))[:-10]
        with self.assertRaises(panel.ProtocolError):
            panel.read_screenshot(io.BytesIO(stream))


class PngEncodeTests(unittest.TestCase):
    def test_signature_and_roundtrip(self):
        w, h = 3, 2
        rgb = bytes([10, 20, 30] * (w * h))
        png = panel.encode_png(w, h, rgb)
        self.assertTrue(png.startswith(b"\x89PNG\r\n\x1a\n"))
        # IHDR is the first chunk; verify width/height/bit-depth/color-type.
        ihdr = png[16:16 + 13]
        pw, ph, depth, ctype = struct.unpack(">IIBB", ihdr[:10])
        self.assertEqual((pw, ph, depth, ctype), (w, h, 8, 2))
        # Decode IDAT and confirm scanlines (filter byte 0 + RGB rows) round-trip.
        idat_start = png.index(b"IDAT") + 4
        idat_len = struct.unpack(">I", png[idat_start - 8:idat_start - 4])[0]
        raw = zlib.decompress(png[idat_start:idat_start + idat_len])
        stride = w * 3
        for y in range(h):
            self.assertEqual(raw[y * (stride + 1)], 0)  # filter byte
            row = raw[y * (stride + 1) + 1:y * (stride + 1) + 1 + stride]
            self.assertEqual(row, rgb[y * stride:(y + 1) * stride])


if __name__ == "__main__":
    unittest.main(verbosity=2)
