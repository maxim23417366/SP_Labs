#!/usr/bin/env python3
"""Автотести для scripts/uart_capture.py: розбір рядків і повний запис через псевдотермінал (без плати)."""

import os
import select
import sys
import tempfile
import threading
import time
import unittest
from pathlib import Path

try:
    import pty  # лише Linux / macOS
except ImportError:  # Windows: тести з псевдотерміналом пропускаються, решта виконується
    pty = None

NEED_PTY = unittest.skipUnless(pty is not None, "потрібен псевдотермінал (pty) — Linux / macOS")

sys.path.insert(0, str(Path(__file__).resolve().parent))

from parse_uart_logs import parse_log_file  # noqa: E402
from uart_capture import (  # noqa: E402
    STLINK_SAFE_BYTES, Capture, build_parser, chunk_bytes, encode_command, load_command,
    pace_after_load, parse_operator, run,
)
from uart_load import build_command_for_step  # noqa: E402

BOARD_OUTPUT = (
    b"BUILD,fw=SmartLock,lab=LR3,git=a1b2c3d,cpu_hz=96000000,loop=superloop\r\n"
    b"EVT t_us=1000000 name=btn_b1_press\r\n"
    b"EVT t_us=1000420 name=btn_b1_handled\r\n"
    b"STATS_BEGIN git=a1b2c3d t_us=4000000 loops=45210\r\n"
    b"STAT name=loop n=45210 min_us=42 avg_us=68 max_us=24500\r\n"
    b"STATS_END\r\n"
)


class CaptureFeedTest(unittest.TestCase):
    def test_lines_split_across_chunks_and_git_from_build(self):
        cap = Capture(t0=0.0)
        lines = cap.feed(BOARD_OUTPUT[:30]) + cap.feed(BOARD_OUTPUT[30:])
        self.assertEqual(len(lines), 6)
        self.assertEqual(cap.git, "a1b2c3d")
        self.assertEqual(cap.rx_bytes, len(BOARD_OUTPUT))
        self.assertTrue(lines[0].startswith("BUILD,"))

    def test_partial_line_kept_until_flush(self):
        cap = Capture(t0=0.0)
        self.assertEqual(cap.feed(b"EVT t_us=1 name=x\r\nPONG seq"), ["EVT t_us=1 name=x"])
        self.assertEqual(cap.flush(), ["PONG seq"])
        self.assertEqual(cap.lines, 2)

    def test_mixed_line_endings_and_empty_lines(self):
        cap = Capture(t0=0.0)
        self.assertEqual(cap.feed(b"a\n\r\nb\rc\r\n"), ["a", "b", "c"])


class OperatorInputTest(unittest.TestCase):
    def test_kinds(self):
        self.assertEqual(parse_operator("  \n", 1)[0], "skip")
        self.assertEqual(parse_operator("# двері відчинено\n", 1), ("note", None, "двері відчинено"))
        self.assertEqual(parse_operator("! PIR рукою\n", 4), ("mark", "MARK 4", "PIR рукою"))
        self.assertEqual(parse_operator("STATS\n", 1), ("send", "STATS", ""))

    def test_encode(self):
        self.assertEqual(encode_command("d"), b"d")
        self.assertEqual(encode_command("STATS"), b"\rSTATS\r\n")


@NEED_PTY
class PtyRunTest(unittest.TestCase):
    """Повний запуск run() на псевдотерміналі замість ST-LINK VCP."""

    def test_capture_writes_raw_ts_conditions_and_sends_commands(self):
        master, slave = pty.openpty()
        port = os.ttyname(slave)
        received = bytearray()

        replies = {
            b"STATS RESET\r\n": b"STATS,reset\r\n",
            b"STATS\r\n": b"STATS_BEGIN git=a1b2c3d t_us=9 loops=1\r\nSTATS_END\r\n",
            b"MEM\r\n": b"STACK reserved=1024 used=720 free=304\r\nHEAP arena=1468 used=1468 free=0\r\n",
        }
        lost = {b"STATS\r\n": 1}  # перший STATS «губиться», як на стенді під час блокувальної задачі

        def board():
            time.sleep(0.3)
            os.write(master, BOARD_OUTPUT)
            pending = b""
            deadline = time.time() + 8.0
            while time.time() < deadline:
                ready, _, _ = select.select([master], [], [], 0.05)
                if not ready:
                    continue
                try:
                    chunk = os.read(master, 256)
                except OSError:
                    break
                received.extend(chunk)
                pending += chunk
                for cmd, reply in replies.items():
                    if pending.endswith(cmd):
                        pending = b""
                        if lost.get(cmd, 0) > 0:
                            lost[cmd] -= 1
                        else:
                            os.write(master, reply)

        with tempfile.TemporaryDirectory() as tmp:
            args = build_parser().parse_args([
                "--port", port, "--name", "T1", "--out", tmp,
                "--duration", "0.02", "--start", "STATS RESET", "--end", "STATS,MEM",
                "--cmd-wait", "0.5", "--no-echo", "--no-input", "--plan", "S1",
                "--boot-settle", "0.1",
            ])
            t = threading.Thread(target=board, daemon=True)
            t.start()
            self.assertEqual(run(args), 0)
            t.join(timeout=10)

            raw = Path(tmp, "T1_uart_raw.txt").read_bytes()
            self.assertTrue(raw.startswith(BOARD_OUTPUT))  # сирий потік байт у байт
            self.assertIn(b"STATS,reset\r\n", raw)
            self.assertIn(b"HEAP arena=1468", raw)

            ts = Path(tmp, "T1_uart_ts.txt").read_text(encoding="utf-8")
            self.assertIn("> STATS RESET", ts)
            self.assertIn("> MEM", ts)
            self.assertIn("S1: не торкатися стенда", ts)
            self.assertIn("немає відповіді на STATS (спроба 1/3)", ts)
            self.assertNotIn("спроба 2/3", ts)

            cond = Path(tmp, "T1_conditions.md").read_text(encoding="utf-8")
            self.assertIn("`a1b2c3d`", cond)
            self.assertNotIn("без відповіді після всіх повторів", cond)

            # сирий файл придатний для парсера метрик
            parsed = parse_log_file(Path(tmp, "T1_uart_raw.txt"))
            self.assertIsNotNone(parsed)

            # повторний запуск з тією ж назвою не затирає файли
            self.assertEqual(run(args), 2)

        os.close(master)
        os.close(slave)
        self.assertIn(b"STATS RESET\r\n", bytes(received))
        self.assertEqual(bytes(received).count(b"STATS\r\n"), 2)  # повтор після втрати
        self.assertIn(b"MEM\r\n", bytes(received))

    def test_command_without_reply_reported(self):
        master, slave = pty.openpty()
        with tempfile.TemporaryDirectory() as tmp:
            args = build_parser().parse_args([
                "--port", os.ttyname(slave), "--name", "T2", "--out", tmp, "--duration", "0.005",
                "--end", "PING 5", "--cmd-wait", "0.2", "--retries", "2", "--no-echo", "--no-input",
            ])
            self.assertEqual(run(args), 0)
            cond = Path(tmp, "T2_conditions.md").read_text(encoding="utf-8")
            self.assertIn("без відповіді після всіх повторів", cond)
            self.assertIn("`PING 5`", cond)
            self.assertIn("не отримано", cond)  # без BUILD git-хеш вписується вручну
        os.close(master)
        os.close(slave)


class LoadModeTest(unittest.TestCase):
    def test_profiles_match_uart_load(self):
        for profile in ("ping", "mixed"):
            for step in range(1, 120):
                self.assertEqual(load_command(step, profile), build_command_for_step(step, profile))

    @NEED_PTY
    def test_s3_capture_and_load_share_one_port(self):
        master, slave = pty.openpty()
        received = bytearray()

        def board():
            time.sleep(0.2)
            os.write(master, BOARD_OUTPUT)
            pending = b""
            deadline = time.time() + 6.0
            while time.time() < deadline:
                ready, _, _ = select.select([master], [], [], 0.02)
                if not ready:
                    continue
                try:
                    chunk = os.read(master, 256)
                except OSError:
                    break
                received.extend(chunk)
                pending += chunk
                while b"\r\n" in pending:
                    line, pending = pending.split(b"\r\n", 1)
                    line = line.lstrip(b"\r")              # encode_command додає провідний \r
                    if line.startswith(b"PING "):
                        os.write(master, b"PONG seq=" + line[5:] + b" t_us=1\r\n")
                    elif line == b"MEM":
                        os.write(master, b"STACK reserved=1024 used=720 free=304\r\nHEAP arena=1 used=1 free=0\r\n")
                    elif line == b"STATS":
                        os.write(master, b"STATS_BEGIN git=a1b2c3d t_us=9 loops=1\r\nSTATS_END\r\n")

        with tempfile.TemporaryDirectory() as tmp:
            args = build_parser().parse_args([
                "--port", os.ttyname(slave), "--name", "S3T", "--out", tmp,
                "--duration", "0.03", "--load-rate", "20", "--load-profile", "ping",
                "--char-gap", "0", "--cmd-wait", "0.5", "--no-echo", "--no-input", "--plan", "S3",
                "--end", "STATS",
            ])
            t = threading.Thread(target=board, daemon=True)
            t.start()
            self.assertEqual(run(args), 0)
            t.join(timeout=8)
            raw = Path(tmp, "S3T_uart_raw.txt").read_bytes()
            self.assertTrue(raw.startswith(BOARD_OUTPUT))         # BUILD з хешем у тому ж лозі
            self.assertIn(b"PONG seq=1 ", raw)
            cond = Path(tmp, "S3T_conditions.md").read_text(encoding="utf-8")
            self.assertIn("`a1b2c3d`", cond)
            self.assertIn("профіль ping, 20 Гц", cond)
            sent = bytes(received).count(b"PING ")
            self.assertGreaterEqual(sent, 20)                     # ≈ 1,8 с × 20 Гц
            self.assertLessEqual(sent, 45)
            self.assertNotIn("`PING 1`", cond)                    # навантаження не засмічує таблицю команд
            parsed = parse_log_file(Path(tmp, "S3T_uart_raw.txt"))
            self.assertIsNotNone(parsed)
        os.close(master)
        os.close(slave)


class ExpectedReplyTest(unittest.TestCase):
    def test_reply_patterns_match_firmware_format(self):
        from uart_capture import expected_reply
        self.assertTrue(expected_reply("STATS").search("STATS_END"))
        self.assertTrue(expected_reply("STATS RESET").search("STATS,reset"))
        self.assertTrue(expected_reply("MEM").search("HEAP arena=1468 used=1468 free=0"))
        self.assertTrue(expected_reply("PING 7").search("PONG seq=7 t_us=123"))
        self.assertFalse(expected_reply("PING 7").search("PONG seq=70 t_us=123"))
        self.assertTrue(expected_reply("MARK 3").search("EVT t_us=1 name=mark_3"))
        self.assertFalse(expected_reply("MARK 3").search("EVT t_us=1 name=mark_33"))
        self.assertIsNone(expected_reply("d"))
        self.assertIsNone(expected_reply("LOG ERASE"))



class LoadChunkTest(unittest.TestCase):
    """M03 (ЛР4): пакети ≤ 25 команд — обхід зависання ST-LINK VCP на потоці > ~380 Б."""

    def test_default_is_off_as_in_lab3(self):
        args = build_parser().parse_args(["--port", "x", "--name", "S3"])
        self.assertEqual(args.load_chunk, 0)
        self.assertEqual(pace_after_load(0, 0, 0.3, 10.0, 10.05), (0, 10.05, False))

    def test_pause_after_each_chunk(self):
        in_chunk, nxt, closed = 0, 0.0, False
        closes = []
        for i in range(60):
            now = i * 0.01
            in_chunk, nxt, closed = pace_after_load(in_chunk, 25, 0.3, now, now)
            if closed:
                closes.append((i + 1, nxt))
        self.assertEqual([n for n, _ in closes], [25, 50])
        self.assertAlmostEqual(closes[0][1], 0.24 + 0.3)

    def test_chunk_of_25_fits_stlink_limit(self):
        for profile in ("ping", "mixed"):
            for first in (1, 251, 9976):
                self.assertLessEqual(chunk_bytes(first, 25, profile), STLINK_SAFE_BYTES)


if __name__ == "__main__":
    unittest.main()