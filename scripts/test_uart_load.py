#!/usr/bin/env python3
"""Автотести для scripts/uart_load.py без фізичної плати (--dry-run та --mock)."""

import tempfile
import unittest
from pathlib import Path

from parse_uart_logs import parse_log_file
from uart_load import main, run_uart_load


class TestUartLoad(unittest.TestCase):
    def test_dry_run_without_hardware(self) -> None:
        res = run_uart_load(
            port=None,
            count=15,
            rate_hz=50.0,
            profile="mixed",
            dry_run=True,
        )
        self.assertEqual(res.mode, "dry-run")
        # 1 (STATS RESET) + 15 команд (з них 1 MEM на кроці 10 і 14 PING) + 1 (STATS) = 17
        self.assertEqual(res.commands_sent, 17)
        self.assertEqual(res.pings_sent, 14)
        self.assertEqual(res.lines_received, 0)
        self.assertIn("STATS RESET", res.sent_preview)
        self.assertIn("MEM", res.sent_preview)

    def test_mock_end_to_end_with_parser(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            log_path = Path(tmp) / "s3_mock.log"
            rc = main(
                [
                    "--mock",
                    "--count",
                    "25",
                    "--profile",
                    "mixed",
                    "--out",
                    str(log_path),
                ]
            )
            self.assertEqual(rc, 0)
            self.assertTrue(log_path.exists())

            # Перевіряємо, що згенерований лог повністю читається парсером M03
            rep = parse_log_file(log_path, scenario="S3")
            self.assertEqual(rep.build["git"], "mock001")
            self.assertIn("uart_cmd_rx->uart_cmd_tx", rep.event_latencies)
            uart_lat = rep.event_latencies["uart_cmd_rx->uart_cmd_tx"]
            self.assertGreater(uart_lat.n, 15)
            self.assertEqual(uart_lat.min_us, 340)
            self.assertIsNotNone(rep.stack_peak)
            self.assertEqual(rep.stack_peak.used, 1416)

    def test_mock_detects_lost_pings(self) -> None:
        res = run_uart_load(
            port=None,
            count=10,
            profile="ping",
            mock=True,
            mock_drop_every=5,
        )
        self.assertEqual(res.pings_sent, 10)
        self.assertEqual(res.pongs_received, 8)
        self.assertEqual(res.lost_pings, 2)

    def test_missing_port_returns_error_code(self) -> None:
        rc = main(["--count", "5"])
        self.assertEqual(rc, 2)


if __name__ == "__main__":
    unittest.main()