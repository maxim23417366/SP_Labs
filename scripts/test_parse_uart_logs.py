#!/usr/bin/env python3
"""Автотести для scripts/parse_uart_logs.py на контрольних логах прошивки I03/I04 (ЛР3) і рядках ЛР4."""

import io
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path

from parse_uart_logs import main, parse_log_file, render_comparison_table, render_side_by_side

# ЛР4: рядки з docs/baseline-lab4.md §5 і методички ЛР4 §6.2–6.5; запис почався до скидання МК
LAB4_LOG = """\
STACK task=logger reserved=1280 used=999 free=281
OSHEAP free=100 min_ever_free=50 total=4096
BUILD,fw=SmartLock,lab=LR4,git=abc1234,cpu_hz=96000000,loop=freertos
EVT t_us=1000 name=btn_b1_press
EVT t_us=1350 name=btn_b1_handled
LOAD pct=12
TRACE_BEGIN n=4 dropped=0
TRACE t_us=10000 task=camera
TRACE t_us=10300 task=buttons
TRACE t_us=10320 task=camera
TRACE t_us=12320 task=IDLE
TRACE_END
STATS_BEGIN git=abc1234 t_us=720000000
STAT name=security n=36000 min_us=900 avg_us=1500 max_us=1700
JITTER name=security period_us=20000 min_us=-12 max_us=310 missed=0
UART rx_overrun=0 cmd_overflow=0 cmd_dropped=0 rx_peak=96
STATS_END
TASKS_BEGIN total=69120000000
camera          41472000000     60%
Tmr Svc         120000          <1%
IDLE            20736000000     30%
TASK name=security prio=5 runtime=6912000000 pct=10
TASKS_END
STACK task=security reserved=896 used=420 free=476
STACK task=security reserved=896 used=380 free=516
STACK task=camera reserved=1408 used=800 free=608
STACK task=MSP reserved=4096 used=512 free=3584
STACK task=env free_min_words=40
STACK task=env free_min_words=44
HEAP arena=1468 used=1468 free=0
OSHEAP free=3000 min_ever_free=2500 total=4096
"""

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


    def test_mem_peak_counts_only_after_reset(self) -> None:
        # Як у S3_uart_raw.txt ЛР3: запис почався до скидання МК
        text = (
            "STACK reserved=4096 used=800 free=3296\n"
            "HEAP arena=2000 used=2000 free=0\n"
            "BUILD,fw=SmartLock,lab=LR3,git=5774b8a,cpu_hz=96000000,loop=superloop\n"
            "STACK reserved=4096 used=720 free=3376\n"
            "HEAP arena=1468 used=1468 free=0\n"
            "STACK reserved=4096 used=760 free=3336\n"
        )
        with tempfile.TemporaryDirectory() as tmp:
            log_path = Path(tmp) / "s3.log"
            log_path.write_text(text, encoding="utf-8")
            rep = parse_log_file(log_path, scenario="S3")
            self.assertEqual(rep.stack_peak.used, 760)
            self.assertEqual(rep.heap_peak.arena, 1468)

    def test_uart_line_with_dma_fields(self) -> None:
        # Формат після PR #42 (DMA): поля cmd_dropped і rx_peak раніше ламали розбір рядка
        with tempfile.TemporaryDirectory() as tmp:
            log_path = Path(tmp) / "u.log"
            log_path.write_text("UART rx_overrun=1 cmd_overflow=0 cmd_dropped=1 rx_peak=2048\n",
                                encoding="utf-8")
            rep = parse_log_file(log_path)
            self.assertEqual(rep.uart_errors["rx_overrun"], 1)
            self.assertEqual(rep.uart_errors["rx_peak"], 2048)
            self.assertEqual(rep.ignored_lines, 0)


class TestLab4Lines(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.path = Path(self._tmp.name) / "S3_lab4.txt"
        self.path.write_text(LAB4_LOG, encoding="utf-8")
        self.rep = parse_log_file(self.path, scenario="S3")

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def test_build_and_stats_begin_without_loops(self) -> None:
        self.assertEqual(self.rep.build["loop"], "freertos")
        self.assertEqual(self.rep.stats_meta["git"], "abc1234")
        self.assertIsNone(self.rep.stats_meta["loops"])
        self.assertEqual(self.rep.jitter["security"].max_us, 310)

    def test_task_stacks_peak_after_reset(self) -> None:
        st = self.rep.stack_tasks
        self.assertNotIn("logger", st)                  # рядок до BUILD відкинуто
        self.assertEqual(st["security"].used, 420)      # пік, а не останнє значення
        self.assertEqual(st["MSP"].reserved, 4096)
        self.assertEqual(self.rep.stack_free_min_words["env"], 40)  # мінімум залишку

    def test_heaps(self) -> None:
        self.assertEqual(self.rep.heap_peak.used, 1468)   # newlib, рядок ЛР3
        self.assertEqual(self.rep.os_heap.used_max, 4096 - 2500)

    def test_run_time_stats(self) -> None:
        tr = self.rep.tasks_runtime
        self.assertEqual(set(tr), {"camera", "Tmr_Svc", "IDLE", "security"})
        self.assertEqual(tr["Tmr_Svc"].pct, 0.5)          # «<1%»
        self.assertEqual(tr["security"].prio, 5)
        self.assertEqual(self.rep.tasks_total, 69120000000)
        self.assertAlmostEqual(self.rep.cpu_from_idle_pct, 70.0, places=1)

    def test_trace_summary(self) -> None:
        ts = self.rep.trace_summary
        self.assertEqual(ts["camera"]["slices"], 2)
        self.assertEqual(ts["camera"]["total_us"], 300 + 2000)
        self.assertEqual(ts["buttons"]["max_slice_us"], 20)

    def test_lab3_and_lab4_side_by_side(self) -> None:
        lab3 = Path(self._tmp.name) / "S3_lab3.txt"
        lab3.write_text(SAMPLE_LOG, encoding="utf-8")
        reports = {"S3_lab3": parse_log_file(lab3), "S3_lab4": self.rep}
        md = render_side_by_side(reports)
        self.assertIn("| Прошивка (`loop`, `git`) | `superloop` `a1b2c3d` | `freertos` `abc1234` |", md)
        self.assertIn("задачі 1220 / 2304 Б; MSP 512 / 4096 Б", md)
        self.assertIn("`uart` → `cmd`, `logger`", md)

    def test_cli_one_run_for_both_labs(self) -> None:
        lab3 = Path(self._tmp.name) / "S3_lab3.txt"
        lab3.write_text(SAMPLE_LOG, encoding="utf-8")
        out_md = Path(self._tmp.name) / "cmp.md"
        buf = io.StringIO()
        with redirect_stdout(buf):
            rc = main(["--log", f"S3_lab3={lab3}", "--log", f"S3_lab4={self.path}", "--md", str(out_md)])
        self.assertEqual(rc, 0)
        text = out_md.read_text(encoding="utf-8")
        self.assertIn("## Порівняння логів поруч", text)
        self.assertIn("### 5. Метрики ОС (ЛР4)", text)


if __name__ == "__main__":
    unittest.main()