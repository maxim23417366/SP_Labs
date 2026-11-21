#!/usr/bin/env python3
"""Тести tools/frame_decode.py без стенда (make tools-test).

Перевіряють розбір змішаного UART-логу, виявлення втрачених рядків і
пошкодженого кадру, PNG і генератор тестового малюнка. Повний шлях із
справжнім кодом прошивки (C → лог → Python → PNG) — ціль tools у
tests/Makefile: test_frame_export пише build/frame_export_host.log, а
frame_decode.py відновлює з нього кадр і звіряє з малюнком.
"""
import contextlib
import io
import os
import sys
import tempfile
import unittest
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import frame_decode as fd  # noqa: E402


def run_cli(args):
    """main() без виводу в консоль (негативні випадки друкують FAIL навмисно)."""
    with contextlib.redirect_stdout(io.StringIO()):
        return fd.main(args)


def make_log(data, event_id=5, chunk=32, noise=True, drop_off=None, flip_off=None):
    """Лог у форматі frame_export.c (CRLF, EVT посередині)."""
    crc = zlib.crc32(data) & 0xFFFFFFFF
    lines = ["BUILD,fw=SmartLock,lab=LR3,git=test",
             "FRAME_BEGIN id=%d seq=0 w=160 h=120 fmt=RGB565 len=%d crc32=%08X chunk=%d"
             % (event_id, len(data), crc, chunk)]
    n = 0
    for off in range(0, len(data), chunk):
        d = bytearray(data[off:off + chunk])
        if flip_off == off:
            d[0] ^= 0x01
        if drop_off != off:
            lines.append("FRM off=%d d=%s" % (off, d.hex().upper()))
        n += 1
        if noise and n % 100 == 0:
            lines.append("EVT t_us=%d name=mark_%d" % (n * 1000, n))
    lines.append("FRAME_END id=%d len=%d crc32=%08X lines=%d" % (event_id, len(data), crc, n))
    lines.append("STATS_END")
    return "\r\n".join(lines) + "\r\n"


class FrameDecodeTest(unittest.TestCase):
    def setUp(self):
        self.data = fd.test_pattern()

    def test_pattern_matches_firmware_reference_pixels(self):
        # Ті самі точки, що перевіряє tests/unit/test_frame_export.c
        self.assertEqual(len(self.data), 38400)
        self.assertEqual(self.data[0:2], b"\xff\xff")                 # біла смуга
        self.assertEqual(self.data[(60 * 160) * 2], 0x00)            # чорний початок градієнта
        self.assertEqual(self.data[-1], 0xFF)                        # білий кінець градієнта
        self.assertEqual(self.data[(20 * 2):(20 * 2) + 2], b"\xff\xe0")  # жовта смуга

    def test_roundtrip_with_noise(self):
        frames = fd.parse_log(make_log(self.data))
        self.assertEqual(len(frames), 1)
        self.assertEqual(frames[0].check(), [])
        self.assertEqual(frames[0].data(), self.data)
        self.assertEqual(frames[0].event_id, 5)
        self.assertEqual(frames[0].end_lines, 1200)

    def test_lost_line_and_corruption_detected(self):
        fr = fd.parse_log(make_log(self.data, drop_off=640))[0]
        self.assertEqual(fr.missing(), [(640, 32)])
        self.assertTrue(any("missing" in p for p in fr.check()))
        fr = fd.parse_log(make_log(self.data, flip_off=320))[0]
        self.assertTrue(any("crc32" in p for p in fr.check()))
        fr = fd.parse_log(make_log(self.data).replace("FRAME_END", "XX"))[0]
        self.assertIn("no FRAME_END (log cut?)", fr.check())

    def test_cli_png_and_pattern(self):
        with tempfile.TemporaryDirectory() as d:
            log = os.path.join(d, "s.log")
            png = os.path.join(d, "s.png")
            with open(log, "w", newline="") as f:
                f.write(make_log(self.data))
            self.assertEqual(run_cli([log, "--out", png, "--expect-test-pattern"]), 0)
            with open(png, "rb") as f:
                head = f.read(24)
            self.assertEqual(head[:8], b"\x89PNG\r\n\x1a\n")
            self.assertEqual(int.from_bytes(head[16:20], "big"), 160)
            self.assertEqual(int.from_bytes(head[20:24], "big"), 120)

            other = bytes(b ^ 0xFF for b in self.data)
            with open(log, "w", newline="") as f:
                f.write(make_log(other))
            self.assertEqual(run_cli([log, "--expect-test-pattern"]), 1)
            with open(log, "w") as f:
                f.write("no frames here\n")
            self.assertEqual(run_cli([log]), 1)

    def test_swap_bytes_for_frames_before_fix(self):
        """Кадр з молодшим байтом першим (як знімала прошивка до виправлення ЛР3, лог 23):
        без --swap-bytes PNG інший («райдужні» смуги), з --swap-bytes — той самий малюнок."""
        old = fd.swap_pairs(self.data)
        self.assertEqual(fd.swap_pairs(old), self.data)
        self.assertEqual(old[0:2], self.data[1::-1])
        with tempfile.TemporaryDirectory() as d:
            log = os.path.join(d, "old.log")
            ref, plain, fixed = (os.path.join(d, n) for n in ("ref.png", "plain.png", "fixed.png"))
            with open(log, "w", newline="") as f:
                f.write(make_log(self.data))
            self.assertEqual(run_cli([log, "--out", ref]), 0)
            with open(log, "w", newline="") as f:
                f.write(make_log(old))
            self.assertEqual(run_cli([log, "--out", plain]), 0)          # CRC цілий
            self.assertEqual(run_cli([log, "--out", fixed, "--swap-bytes"]), 0)
            with open(ref, "rb") as a, open(plain, "rb") as b, open(fixed, "rb") as c:
                r, p_, f_ = a.read(), b.read(), c.read()
            self.assertNotEqual(r, p_)
            self.assertEqual(r, f_)

    def test_rgb565_conversion(self):
        rows = fd.rgb565_be_to_rgb888(bytes([0xF8, 0x00, 0x07, 0xE0, 0x00, 0x1F, 0xFF, 0xFF]), 4, 1)
        self.assertEqual(rows[0], bytes([255, 0, 0, 0, 255, 0, 0, 0, 255, 255, 255, 255]))


if __name__ == "__main__":
    unittest.main(verbosity=2)
