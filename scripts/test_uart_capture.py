#!/usr/bin/env python3
"""Автотести для scripts/uart_capture.py: розбір рядків і повний запис через псевдотермінал (без плати)."""

import os
import pty
import select
import sys
import tempfile
import threading
import time
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from parse_uart_logs import parse_log_file  # noqa: E402
from uart_capture import Capture, build_parser, encode_command, parse_operator, run  # noqa: E402

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


if __name__ == "__main__":
    unittest.main()
