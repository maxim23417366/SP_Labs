#!/usr/bin/env python3
"""Автотести для scripts/parse_uart_logs.py на контрольних логах прошивки I03/I04."""

import tempfile
import unittest
from pathlib import Path

from parse_uart_logs import parse_log_file, render_comparison_table

SAMPLE_LOG = """\
BUILD,fw=SmartLock,lab=LR3,git=a1b2c3d,cpu_hz=96000000,loop=superloop
# LR3: STATS [RAW|RESET] | MEM | PING <n> | MARK <n> | HELP
STACK reserved=2048 used=612 free=1436
HEAP arena=1024 used=256 free=768
EVT t_us=1000000 name=mark_1
EVT t_us=1010000 name=btn_b1_press
EVT t_us=1010420 name=btn_b1_handled
EVT t_us=1010510 name=fsm_lock_request
LOAD pct=12
EVT t_us=2010000 name=btn_b1_press
EVT t_us=2011420 name=btn_b1_handled
EVT t_us=2011600 name=fsm_lock_request
LOAD pct=18
EVT t_us=3000000 name=uart_cmd_rx
EVT t_us=3003500 name=uart_cmd_tx
STACK reserved=2048 used=1384 free=664
HEAP arena=1024 used=256 free=768
STATS_BEGIN git=a1b2c3d t_us=4000000 loops=45210
STAT name=loop n=45210 min_us=42 avg_us=68 max_us=24500
CPU name=cpu_load windows=4 min_pm=110 avg_pm=145 max_pm=185
STAT name=fsm n=12 min_us=15 avg_us=22 max_us=45
POLL name=fsm polls=45210 runs=12
STAT name=buttons n=400 min_us=18 avg_us=25 max_us=60
JITTER name=buttons period_us=10000 min_us=-12 max_us=14500 missed=1
JHIST name=buttons n=399 h_lt100us=390 h_lt1ms=8 h_lt10ms=0 h_lt100ms=1 h_ge100ms=0
STAT name=btn_b1_lat n=2 min_us=420 avg_us=920 max_us=1420
OVR name=btn_b1 count=1
STAT name=evq_wait n=2 min_us=90 avg_us=135 max_us=180
EVQ len=8 pending=0 max_depth=1 posted=2 dropped=0
UART rx_overrun=0 cmd_overflow=0
EVTLOG pending=0 dropped=0
STACK reserved=2048 used=1384 free=664
HEAP arena=1024 used=256 free=768
STATS_END
"""


class TestParseUartLogs(unittest.TestCase):
    def test_parses_all_sections_and_p99(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            log_path = Path(tmp) / "s2.log"
            log_path.write_text(SAMPLE_LOG, encoding="utf-8")

            rep = parse_log_file(log_path, scenario="S2")

            self.assertEqual(rep.build["git"], "a1b2c3d")
            self.assertEqual(rep.cpu_hz, 96_000_000)
            self.assertEqual(rep.stats["loop"].max_us, 24500)
            self.assertEqual(rep.jitter["buttons"].min_us, -12)
            self.assertEqual(rep.jitter["buttons"].missed, 1)
            self.assertEqual(rep.jhist["buttons"].h_lt100ms, 1)
            self.assertEqual(rep.overruns["btn_b1"], 1)
            self.assertEqual(rep.load_summary["avg_pct"], 15.0)
            self.assertEqual(rep.stack_peak.used, 1384)
            self.assertEqual(len(rep.marks), 1)

            b1 = rep.event_latencies["btn_b1_press->btn_b1_handled"]
            self.assertEqual(b1.n, 2)
            self.assertEqual(b1.min_us, 420)
            self.assertEqual(b1.max_us, 1420)
            self.assertAlmostEqual(b1.p99_us, 1410.0, places=1)

            table = render_comparison_table({"S2": rep})
            self.assertIn("24500 мкс", table)
            self.assertIn("1384 / 2048 Б", table)

    def test_raw_cycles_conversion(self) -> None:
        raw_text = (
            "BUILD,fw=SmartLock,lab=LR3,git=deadbee,cpu_hz=96000000,loop=superloop\n"
            "STAT name=loop n=10 min_cyc=960 avg_cyc=1920 max_cyc=9600\n"
        )
        with tempfile.TemporaryDirectory() as tmp:
            log_path = Path(tmp) / "raw.log"
            log_path.write_text(raw_text, encoding="utf-8")

            rep = parse_log_file(log_path)
            self.assertTrue(rep.stats["loop"].raw_cycles)
            self.assertEqual(rep.stats["loop"].min_us, 10)
            self.assertEqual(rep.stats["loop"].avg_us, 20)
            self.assertEqual(rep.stats["loop"].max_us, 100)


if __name__ == "__main__":
    unittest.main()