#!/usr/bin/env python3
"""
Парсер та аналізатор діагностичних UART-логів SmartLock (ЛР3 — M03, ЛР4 — M03).

Один і той самий скрипт обробляє логи обох прошивок: super loop (тег lab3,
банер loop=superloop) і FreeRTOS (loop=freertos). Рядки ЛР3 розбираються без
змін, рядки ЛР4 лише додаються (docs/baseline-lab4.md §5, рецензія K01 у PR #68):

  STACK task=<ім'я> reserved=<Б> used=<Б> free=<Б>   стек задачі; task=MSP — стек переривань
  STACK task=<ім'я> free_min_words=<слів>            варіант з методички ЛР4 §6.3
  OSHEAP free=<Б> min_ever_free=<Б> total=<Б>       heap ОС (HEAP free=… — варіант методички §6.4)
  TASKS_BEGIN [total=<такти>] … TASKS_END            run-time stats (методичка ЛР4 §6.2):
      TASK name=<ім'я> [prio=<p>] runtime=<такти> pct=<%>   або сирий рядок vTaskGetRunTimeStats
  TRACE t_us=<мкс> task=<ім'я>                       перемикання контексту (traceTASK_SWITCHED_IN)

Узгоджено з:
- docs/metrics/metrics-spec.md (§2, §3, §5)
- firmware/App/stats.c, evt.c, app_tasks.c, cmd.c, app.c (картки I03, I04, I06)
"""

from __future__ import annotations

import argparse
import csv
import json
import math
import re
import sys
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Dict, List, Optional, Tuple

DEFAULT_CPU_HZ = 96_000_000

# Стандартні пари подій EVT (start_event -> end_event) для розрахунку M-03 (включно з p99)
DEFAULT_EVT_PAIRS: List[Tuple[str, str, str]] = [
    ("btn_b1_press", "btn_b1_handled", "B1 ISR -> task buttons"),
    ("btn_b1_press", "fsm_lock_request", "B1 ISR -> FSM take"),
    ("pir_trig", "pir_handled", "PIR ISR -> task security"),
    ("pir_trig", "fsm_motion", "PIR ISR -> FSM take"),
    ("pir_trig", "alarm_triggered", "PIR ISR -> alarm_triggered"),
    ("mpu_int", "mpu_handled", "MPU ISR -> task security"),
    ("mpu_int", "fsm_tamper", "MPU ISR -> FSM take"),
    ("code_key_press", "code_key_handled", "Code key -> handled"),
    ("uart_cmd_rx", "uart_cmd_tx", "UART command roundtrip"),
    ("capture_start", "capture_done", "OV7670 frame capture"),
    ("flash_wr_start", "flash_wr_done", "W25Q64 write operation"),
]

# Регулярні вирази за контрактом stats.c / evt.c / app.c / app_tasks.c
RE_BUILD = re.compile(
    r"^BUILD,fw=(?P<fw>[^,]+),lab=(?P<lab>[^,]+),git=(?P<git>[^,]+),"
    r"cpu_hz=(?P<cpu_hz>\d+),loop=(?P<loop>\S+)$"
)
RE_STATS_BEGIN = re.compile(
    r"^STATS_BEGIN git=(?P<git>\S+) t_us=(?P<t_us>\d+)(?: loops=(?P<loops>\d+))?$"
)
RE_STATS_END = re.compile(r"^STATS_END$")
RE_STAT_US = re.compile(
    r"^STAT name=(?P<name>\S+) n=(?P<n>\d+) "
    r"min_us=(?P<min>\d+) avg_us=(?P<avg>\d+) max_us=(?P<max>\d+)$"
)
RE_STAT_CYC = re.compile(
    r"^STAT name=(?P<name>\S+) n=(?P<n>\d+) "
    r"min_cyc=(?P<min>\d+) avg_cyc=(?P<avg>\d+) max_cyc=(?P<max>\d+)$"
)
RE_CPU = re.compile(
    r"^CPU name=(?P<name>\S+) windows=(?P<windows>\d+) "
    r"min_pm=(?P<min_pm>\d+) avg_pm=(?P<avg_pm>\d+) max_pm=(?P<max_pm>\d+)$"
)
RE_JITTER = re.compile(
    r"^JITTER name=(?P<name>\S+) period_us=(?P<period_us>\d+) "
    r"min_us=(?P<min_us>-?\d+) max_us=(?P<max_us>-?\d+) missed=(?P<missed>\d+)$"
)
RE_JHIST = re.compile(
    r"^JHIST name=(?P<name>\S+) n=(?P<n>\d+) "
    r"h_lt100us=(?P<h_lt100us>\d+) h_lt1ms=(?P<h_lt1ms>\d+) "
    r"h_lt10ms=(?P<h_lt10ms>\d+) h_lt100ms=(?P<h_lt100ms>\d+) h_ge100ms=(?P<h_ge100ms>\d+)$"
)
RE_POLL = re.compile(
    r"^POLL name=(?P<name>\S+) polls=(?P<polls>\d+) runs=(?P<runs>\d+)$"
)
RE_OVR = re.compile(r"^OVR name=(?P<name>\S+) count=(?P<count>\d+)$")
RE_EVQ = re.compile(
    r"^EVQ len=(?P<len>\d+) pending=(?P<pending>\d+) max_depth=(?P<max_depth>\d+) "
    r"posted=(?P<posted>\d+) dropped=(?P<dropped>\d+)$"
)
# UART: з PR #42 (DMA) додались cmd_dropped і rx_peak — приймаємо будь-які поля ключ=число
RE_UART = re.compile(r"^UART (?P<fields>\w+=\d+(?: \w+=\d+)*)$")
RE_EVTLOG = re.compile(r"^EVTLOG pending=(?P<pending>\d+) dropped=(?P<dropped>\d+)$")
RE_LOAD = re.compile(r"^LOAD pct=(?P<pct>\d+)$")
RE_EVT = re.compile(r"^EVT t_us=(?P<t_us>\d+) name=(?P<name>\S+)$")
RE_STACK = re.compile(
    r"^STACK reserved=(?P<reserved>\d+) used=(?P<used>\d+) free=(?P<free>\d+)$"
)
RE_HEAP = re.compile(
    r"^HEAP arena=(?P<arena>\d+) used=(?P<used>\d+) free=(?P<free>\d+)$"
)

# ---- ЛР4 (FreeRTOS): нові рядки, рядки ЛР3 не змінюються ----
RE_STACK_TASK = re.compile(
    r"^STACK task=(?P<task>\S+) reserved=(?P<reserved>\d+) used=(?P<used>\d+) free=(?P<free>\d+)$"
)
RE_STACK_WORDS = re.compile(r"^STACK task=(?P<task>\S+) free_min_words=(?P<words>\d+)$")
RE_OSHEAP = re.compile(
    r"^(?:OSHEAP|HEAP) free=(?P<free>\d+) min_ever_free=(?P<min_ever_free>\d+) total=(?P<total>\d+)$"
)
RE_TASKS_BEGIN = re.compile(r"^TASKS_BEGIN(?: total=(?P<total>\d+))?$")
RE_TASKS_END = re.compile(r"^TASKS_END$")
RE_TASK = re.compile(
    r"^TASK name=(?P<name>\S+)(?: prio=(?P<prio>\d+))? runtime=(?P<runtime>\d+) "
    r"pct=(?P<pct><?\d+(?:\.\d+)?)%?$"
)
# Сирий рядок vTaskGetRunTimeStats: «ім'я<таб>абсолютний час<таб>відсоток%» (ім'я може мати пробіл: «Tmr Svc»)
RE_TASK_RAW = re.compile(r"^(?P<name>\S(?:.*?\S)?)\s+(?P<runtime>\d+)\s+(?P<pct><?\d+)%$")
RE_TRACE = re.compile(r"^TRACE t_us=(?P<t_us>\d+) task=(?P<task>\S+)$")
RE_TRACE_META = re.compile(r"^TRACE_(?:BEGIN|END)(?: .*)?$")

IDLE_TASK_NAMES = ("IDLE", "IDLE0")

# Імена задач ЛР3 -> ЛР4 (docs/rtos-design.md §2): uart ділиться на cmd і logger,
# mem переходить у logger, головного циклу (loop) у ЛР4 немає
TASK_ALIASES_LAB3_TO_LAB4: Dict[str, Tuple[str, ...]] = {
    "uart": ("cmd", "logger"),
    "mem": ("logger",),
    "loop": (),
}


@dataclass
class StatRecord:
    name: str
    n: int
    min_us: int
    avg_us: int
    max_us: int
    raw_cycles: bool = False


@dataclass
class JitterRecord:
    name: str
    period_us: int
    min_us: int
    max_us: int
    missed: int


@dataclass
class JitterHistRecord:
    name: str
    n: int
    h_lt100us: int
    h_lt1ms: int
    h_lt10ms: int
    h_lt100ms: int
    h_ge100ms: int


@dataclass
class EventPairSummary:
    pair_id: str
    description: str
    start_event: str
    end_event: str
    n: int
    min_us: int
    avg_us: float
    max_us: int
    p99_us: float
    samples_us: List[int] = field(default_factory=list)


@dataclass
class MemSnapshot:
    reserved: int = 0
    used: int = 0
    free: int = 0


@dataclass
class HeapSnapshot:
    arena: int = 0
    used: int = 0
    free: int = 0


@dataclass
class TaskRuntime:
    name: str
    runtime: int
    pct: float          # з рядка прошивки (для «<1%» — 0.5)
    prio: Optional[int] = None


@dataclass
class OsHeapSnapshot:
    free: int = 0
    min_ever_free: int = 0
    total: int = 0

    @property
    def used_max(self) -> int:
        """Найбільше використання за весь час: total − min_ever_free (методичка ЛР4 §6.4)."""
        return self.total - self.min_ever_free


@dataclass
class LogReport:
    source_file: str
    scenario: str
    build: Dict[str, str] = field(default_factory=dict)
    cpu_hz: int = DEFAULT_CPU_HZ
    stats_meta: Dict[str, str] = field(default_factory=dict)
    stats: Dict[str, StatRecord] = field(default_factory=dict)
    cpu_stat_pm: Dict[str, int] = field(default_factory=dict)
    jitter: Dict[str, JitterRecord] = field(default_factory=dict)
    jhist: Dict[str, JitterHistRecord] = field(default_factory=dict)
    polls: Dict[str, Dict[str, int]] = field(default_factory=dict)
    overruns: Dict[str, int] = field(default_factory=dict)
    evq: Dict[str, int] = field(default_factory=dict)
    uart_errors: Dict[str, int] = field(default_factory=dict)
    evtlog: Dict[str, int] = field(default_factory=dict)
    load_samples_pct: List[int] = field(default_factory=list)
    load_summary: Dict[str, float] = field(default_factory=dict)
    evt_count: int = 0
    marks: List[Tuple[int, str]] = field(default_factory=list)
    event_latencies: Dict[str, EventPairSummary] = field(default_factory=dict)
    stack_peak: Optional[MemSnapshot] = None
    heap_peak: Optional[HeapSnapshot] = None
    # ЛР4
    stack_tasks: Dict[str, MemSnapshot] = field(default_factory=dict)
    stack_free_min_words: Dict[str, int] = field(default_factory=dict)
    os_heap: Optional[OsHeapSnapshot] = None
    tasks_total: Optional[int] = None
    tasks_runtime: Dict[str, TaskRuntime] = field(default_factory=dict)
    cpu_from_idle_pct: Optional[float] = None
    trace_events: List[Tuple[int, str]] = field(default_factory=list)
    trace_summary: Dict[str, Dict[str, float]] = field(default_factory=dict)
    parsed_lines: int = 0
    ignored_lines: int = 0
    warnings: List[str] = field(default_factory=list)


def percentile(values: List[int], q: float) -> float:
    """Обчислення q-го перцентиля (0..100) методом лінійної інтерполяції."""
    if not values:
        return 0.0
    ordered = sorted(values)
    if len(ordered) == 1:
        return float(ordered[0])
    pos = (len(ordered) - 1) * (q / 100.0)
    lo = math.floor(pos)
    hi = math.ceil(pos)
    if lo == hi:
        return float(ordered[lo])
    weight = pos - lo
    return ordered[lo] * (1.0 - weight) + ordered[hi] * weight


def cycles_to_us(cycles: int, cpu_hz: int) -> int:
    if cpu_hz <= 0:
        return 0
    return int(round((cycles * 1_000_000) / cpu_hz))


def parse_log_file(
    path: Path,
    scenario: str = "RUN",
    extra_pairs: Optional[List[Tuple[str, str, str]]] = None,
) -> LogReport:
    report = LogReport(source_file=str(path), scenario=scenario)
    pairs = list(DEFAULT_EVT_PAIRS)
    if extra_pairs:
        pairs.extend(extra_pairs)

    # Індекс: start_event -> список пар (pair_id, end_event)
    starts_map: Dict[str, List[Tuple[str, str]]] = {}
    ends_map: Dict[str, List[Tuple[str, str]]] = {}
    for start_ev, end_ev, desc in pairs:
        pair_id = f"{start_ev}->{end_ev}"
        starts_map.setdefault(start_ev, []).append((pair_id, end_ev))
        ends_map.setdefault(end_ev, []).append((pair_id, start_ev))

    pending_starts: Dict[str, int] = {}
    raw_deltas: Dict[str, List[int]] = {f"{s}->{e}": [] for s, e, _ in pairs}
    pair_meta: Dict[str, Tuple[str, str, str]] = {
        f"{s}->{e}": (s, e, d) for s, e, d in pairs
    }

    in_tasks = False
    text = path.read_text(encoding="utf-8", errors="replace")
    for line_no, raw_line in enumerate(text.splitlines(), start=1):
        line = raw_line.strip()
        if not line:
            continue

        m = RE_BUILD.match(line)
        if m:
            report.build = m.groupdict()
            report.cpu_hz = int(m.group("cpu_hz"))
            pending_starts.clear()  # скидаємо незавершені пари після рестарту МК
            # Stack painting, heap і лічильники ОС рахуються від скидання МК: рядки
            # до банера BUILD — попередній сеанс плати (у S3 ЛР3 — 800 Б замість 760 Б).
            # Команда VER теж друкує BUILD; це не шкодить: наступні STACK / HEAP /
            # OSHEAP знову покажуть накопичений від старту максимум.
            report.stack_peak = None
            report.heap_peak = None
            report.stack_tasks.clear()
            report.stack_free_min_words.clear()
            report.os_heap = None
            in_tasks = False
            report.parsed_lines += 1
            continue

        # ---- ЛР4: run-time stats (блок TASKS_BEGIN … TASKS_END) ----
        m = RE_TASKS_BEGIN.match(line)
        if m:
            in_tasks = True
            report.tasks_runtime = {}
            report.tasks_total = int(m.group("total")) if m.group("total") else None
            report.parsed_lines += 1
            continue
        if in_tasks:
            if RE_TASKS_END.match(line):
                in_tasks = False
                report.parsed_lines += 1
                continue
            m = RE_TASK.match(line) or RE_TASK_RAW.match(line)
            if m:
                pct_txt = m.group("pct")
                pct = 0.5 if pct_txt.startswith("<") else float(pct_txt)
                prio = m.groupdict().get("prio")
                name = m.group("name").replace(" ", "_")
                report.tasks_runtime[name] = TaskRuntime(
                    name=name, runtime=int(m.group("runtime")), pct=pct,
                    prio=int(prio) if prio else None,
                )
                report.parsed_lines += 1
                continue

        m = RE_STACK_TASK.match(line)
        if m:
            task = m.group("task")
            snap = MemSnapshot(
                reserved=int(m.group("reserved")),
                used=int(m.group("used")),
                free=int(m.group("free")),
            )
            prev = report.stack_tasks.get(task)
            if prev is None or snap.used >= prev.used:
                report.stack_tasks[task] = snap
            report.parsed_lines += 1
            continue

        m = RE_STACK_WORDS.match(line)
        if m:
            task = m.group("task")
            words = int(m.group("words"))
            prev_w = report.stack_free_min_words.get(task)
            report.stack_free_min_words[task] = words if prev_w is None else min(prev_w, words)
            report.parsed_lines += 1
            continue

        m = RE_OSHEAP.match(line)
        if m:
            snap_h = OsHeapSnapshot(
                free=int(m.group("free")),
                min_ever_free=int(m.group("min_ever_free")),
                total=int(m.group("total")),
            )
            if report.os_heap is None or snap_h.min_ever_free <= report.os_heap.min_ever_free:
                report.os_heap = snap_h
            report.parsed_lines += 1
            continue

        m = RE_TRACE.match(line)
        if m:
            report.trace_events.append((int(m.group("t_us")), m.group("task")))
            report.parsed_lines += 1
            continue
        if RE_TRACE_META.match(line):
            report.parsed_lines += 1
            continue

        m = RE_STATS_BEGIN.match(line)
        if m:
            report.stats_meta = m.groupdict()
            report.parsed_lines += 1
            continue

        if RE_STATS_END.match(line):
            report.parsed_lines += 1
            continue

        m = RE_STAT_US.match(line)
        if m:
            name = m.group("name")
            report.stats[name] = StatRecord(
                name=name,
                n=int(m.group("n")),
                min_us=int(m.group("min")),
                avg_us=int(m.group("avg")),
                max_us=int(m.group("max")),
                raw_cycles=False,
            )
            report.parsed_lines += 1
            continue

        m = RE_STAT_CYC.match(line)
        if m:
            name = m.group("name")
            report.stats[name] = StatRecord(
                name=name,
                n=int(m.group("n")),
                min_us=cycles_to_us(int(m.group("min")), report.cpu_hz),
                avg_us=cycles_to_us(int(m.group("avg")), report.cpu_hz),
                max_us=cycles_to_us(int(m.group("max")), report.cpu_hz),
                raw_cycles=True,
            )
            report.parsed_lines += 1
            continue

        m = RE_CPU.match(line)
        if m:
            report.cpu_stat_pm = {
                "windows": int(m.group("windows")),
                "min_pm": int(m.group("min_pm")),
                "avg_pm": int(m.group("avg_pm")),
                "max_pm": int(m.group("max_pm")),
            }
            report.parsed_lines += 1
            continue

        m = RE_JITTER.match(line)
        if m:
            name = m.group("name")
            report.jitter[name] = JitterRecord(
                name=name,
                period_us=int(m.group("period_us")),
                min_us=int(m.group("min_us")),
                max_us=int(m.group("max_us")),
                missed=int(m.group("missed")),
            )
            report.parsed_lines += 1
            continue

        m = RE_JHIST.match(line)
        if m:
            name = m.group("name")
            report.jhist[name] = JitterHistRecord(
                name=name,
                n=int(m.group("n")),
                h_lt100us=int(m.group("h_lt100us")),
                h_lt1ms=int(m.group("h_lt1ms")),
                h_lt10ms=int(m.group("h_lt10ms")),
                h_lt100ms=int(m.group("h_lt100ms")),
                h_ge100ms=int(m.group("h_ge100ms")),
            )
            report.parsed_lines += 1
            continue

        m = RE_POLL.match(line)
        if m:
            report.polls[m.group("name")] = {
                "polls": int(m.group("polls")),
                "runs": int(m.group("runs")),
            }
            report.parsed_lines += 1
            continue

        m = RE_OVR.match(line)
        if m:
            report.overruns[m.group("name")] = int(m.group("count"))
            report.parsed_lines += 1
            continue

        m = RE_EVQ.match(line)
        if m:
            report.evq = {k: int(v) for k, v in m.groupdict().items()}
            report.parsed_lines += 1
            continue

        m = RE_UART.match(line)
        if m:
            report.uart_errors = {
                k: int(v) for k, v in (kv.split("=") for kv in m.group("fields").split())
            }
            report.parsed_lines += 1
            continue

        m = RE_EVTLOG.match(line)
        if m:
            report.evtlog = {k: int(v) for k, v in m.groupdict().items()}
            report.parsed_lines += 1
            continue

        m = RE_LOAD.match(line)
        if m:
            report.load_samples_pct.append(int(m.group("pct")))
            report.parsed_lines += 1
            continue

        m = RE_STACK.match(line)
        if m:
            snap = MemSnapshot(
                reserved=int(m.group("reserved")),
                used=int(m.group("used")),
                free=int(m.group("free")),
            )
            if report.stack_peak is None or snap.used >= report.stack_peak.used:
                report.stack_peak = snap
            report.parsed_lines += 1
            continue

        m = RE_HEAP.match(line)
        if m:
            hsnap = HeapSnapshot(
                arena=int(m.group("arena")),
                used=int(m.group("used")),
                free=int(m.group("free")),
            )
            if report.heap_peak is None or hsnap.used >= report.heap_peak.used:
                report.heap_peak = hsnap
            report.parsed_lines += 1
            continue

        m = RE_EVT.match(line)
        if m:
            t_us = int(m.group("t_us"))
            ev_name = m.group("name")
            report.evt_count += 1
            report.parsed_lines += 1

            if ev_name == "stats_reset":
                pending_starts.clear()
                continue

            if ev_name.startswith("mark_"):
                report.marks.append((t_us, ev_name))

            # Спочатку закриваємо очікувані кінцеві події
            if ev_name in ends_map:
                for pair_id, _ in ends_map[ev_name]:
                    if pair_id in pending_starts:
                        t0 = pending_starts.pop(pair_id)
                        dt = t_us - t0
                        if dt >= 0:
                            raw_deltas[pair_id].append(dt)
                        else:
                            report.warnings.append(
                                f"Рядок {line_no}: від'ємний інтервал {dt} мкс для {pair_id}"
                            )

            # Потім фіксуємо початкові мітки часу
            if ev_name in starts_map:
                for pair_id, _ in starts_map[ev_name]:
                    pending_starts[pair_id] = t_us
            continue

        # Службові рядки (# коментарі, FRM, PONG, self-test ЛР2) ігноруються без збою
        report.ignored_lines += 1

    # Агрегація LOAD
    if report.load_samples_pct:
        vals = report.load_samples_pct
        report.load_summary = {
            "n": len(vals),
            "min_pct": float(min(vals)),
            "avg_pct": round(sum(vals) / len(vals), 2),
            "max_pct": float(max(vals)),
        }
    elif report.cpu_stat_pm:
        report.load_summary = {
            "n": float(report.cpu_stat_pm["windows"]),
            "min_pct": round(report.cpu_stat_pm["min_pm"] / 10.0, 2),
            "avg_pct": round(report.cpu_stat_pm["avg_pm"] / 10.0, 2),
            "max_pct": round(report.cpu_stat_pm["max_pm"] / 10.0, 2),
        }

    # Агрегація пар EVT
    for pair_id, deltas in raw_deltas.items():
        if deltas:
            start_ev, end_ev, desc = pair_meta[pair_id]
            report.event_latencies[pair_id] = EventPairSummary(
                pair_id=pair_id,
                description=desc,
                start_event=start_ev,
                end_event=end_ev,
                n=len(deltas),
                min_us=min(deltas),
                avg_us=round(sum(deltas) / len(deltas), 2),
                max_us=max(deltas),
                p99_us=round(percentile(deltas, 99.0), 2),
                samples_us=deltas,
            )

    # ЛР4: завантаження CPU як 100 − частка IDLE (рахується за абсолютним часом, а не
    # за округленим відсотком прошивки)
    if report.tasks_runtime:
        total_rt = sum(t.runtime for t in report.tasks_runtime.values())
        idle = [t for n, t in report.tasks_runtime.items() if n in IDLE_TASK_NAMES]
        if idle and total_rt > 0:
            idle_rt = sum(t.runtime for t in idle)
            report.cpu_from_idle_pct = round(100.0 * (1.0 - idle_rt / total_rt), 2)

    # ЛР4: хронологія задач — кожна подія TRACE означає «задача отримала CPU»;
    # її відрізок триває до наступного перемикання
    if len(report.trace_events) >= 2:
        ev = sorted(report.trace_events)
        window = ev[-1][0] - ev[0][0]
        summ: Dict[str, Dict[str, float]] = {}
        for (t0, task), (t1, _) in zip(ev, ev[1:]):
            d = summ.setdefault(task, {"slices": 0, "total_us": 0, "max_slice_us": 0})
            dt = t1 - t0
            d["slices"] += 1
            d["total_us"] += dt
            d["max_slice_us"] = max(d["max_slice_us"], dt)
        for d in summ.values():
            d["share_pct"] = round(100.0 * d["total_us"] / window, 2) if window > 0 else 0.0
        report.trace_summary = summ

    if report.evtlog.get("dropped", 0) > 0:
        report.warnings.append(
            f"Увага: кільцевий буфер EVT втратив {report.evtlog['dropped']} подій (EVTLOG dropped > 0)"
        )
    if report.evq.get("dropped", 0) > 0:
        report.warnings.append(
            f"Увага: черга FSM втратила {report.evq['dropped']} подій (EVQ dropped > 0)"
        )
    if report.uart_errors.get("rx_overrun", 0) > 0:
        report.warnings.append(
            f"Увага: переповнення UART RX ({report.uart_errors['rx_overrun']})"
        )

    return report


def _fmt_us(val: Optional[float | int]) -> str:
    if val is None:
        return "—"
    if isinstance(val, float):
        return f"{val:.1f} мкс"
    return f"{val} мкс"


def render_single_markdown(rep: LogReport) -> str:
    lines: List[str] = [
        f"## Звіт аналізу UART-логу ({rep.scenario})",
        "",
        f"* **Файл:** `{rep.source_file}`",
        f"* **Збірка:** `git={rep.build.get('git', rep.stats_meta.get('git', 'unknown'))}`, "
        f"`cpu_hz={rep.cpu_hz}`, `loop={rep.build.get('loop', '?')}`",
        f"* **Рядків оброблено / пропущено:** `{rep.parsed_lines}` / `{rep.ignored_lines}` "
        f"(подій `EVT`: `{rep.evt_count}`)",
        "",
    ]

    if rep.warnings:
        lines.append("### Попередження")
        for w in rep.warnings:
            lines.append(f"* {w}")
        lines.append("")

    if rep.stats:
        lines.extend(
            [
                "### 1. Статистика задач, циклу та затримок (`STAT`)",
                "",
                "| Ім'я (`name`) | К-сть (`n`) | `min_us` | `avg_us` | `max_us` |",
                "| :--- | :---: | :---: | :---: | :---: |",
            ]
        )
        for st in rep.stats.values():
            lines.append(
                f"| `{st.name}` | {st.n} | {st.min_us} | {st.avg_us} | {st.max_us} |"
            )
        lines.append("")

    if rep.event_latencies:
        lines.extend(
            [
                "### 2. Затримки за парними подіями `EVT` (з розрахунком `p99`)",
                "",
                "| Ланцюжок подій | Опис | `n` | `min_us` | `avg_us` | `max_us` | **`p99_us`** |",
                "| :--- | :--- | :---: | :---: | :---: | :---: | :---: |",
            ]
        )
        for ev in rep.event_latencies.values():
            lines.append(
                f"| `{ev.pair_id}` | {ev.description} | {ev.n} | "
                f"{ev.min_us} | {ev.avg_us:.1f} | {ev.max_us} | **{ev.p99_us:.1f}** |"
            )
        lines.append("")

    if rep.jitter:
        lines.extend(
            [
                "### 3. Джитер періодичних задач (`JITTER` / `JHIST`)",
                "",
                "| Задача | Період (мкс) | `min_us` | `max_us` | `missed` | Гістограма (`<100us / <1ms / <10ms / <100ms / >=100ms`) |",
                "| :--- | :---: | :---: | :---: | :---: | :---: |",
            ]
        )
        for name, jit in rep.jitter.items():
            jh = rep.jhist.get(name)
            hist_str = (
                f"{jh.h_lt100us} / {jh.h_lt1ms} / {jh.h_lt10ms} / {jh.h_lt100ms} / {jh.h_ge100ms}"
                if jh
                else "—"
            )
            lines.append(
                f"| `{name}` | {jit.period_us} | {jit.min_us} | {jit.max_us} | {jit.missed} | {hist_str} |"
            )
        lines.append("")

    if rep.load_summary or rep.stack_peak or rep.heap_peak or rep.evq or rep.uart_errors:
        lines.extend(["### 4. Ресурси CPU, пам'яті та стан черг", ""])
        if rep.load_summary:
            ls = rep.load_summary
            lines.append(
                f"* **CPU Load:** min = `{ls['min_pct']}%`, avg = `{ls['avg_pct']}%`, "
                f"max = `{ls['max_pct']}%` (вибірок: `{int(ls['n'])}`)"
            )
        if rep.stack_peak:
            sp = rep.stack_peak
            lines.append(
                f"* **STACK (пік):** reserved = `{sp.reserved}` Б, used = `{sp.used}` Б, free = `{sp.free}` Б"
            )
        if rep.heap_peak:
            hp = rep.heap_peak
            lines.append(
                f"* **HEAP (пік):** arena = `{hp.arena}` Б, used = `{hp.used}` Б, free = `{hp.free}` Б"
            )
        if rep.evq:
            lines.append(f"* **EVQ:** `{rep.evq}`")
        if rep.uart_errors:
            lines.append(f"* **UART errors:** `{rep.uart_errors}`")
        if rep.overruns:
            lines.append(f"* **ISR overruns (`OVR`):** `{rep.overruns}`")
        lines.append("")

    lines.extend(render_os_markdown(rep))
    return "\n".join(lines)


def render_os_markdown(rep: LogReport) -> List[str]:
    """Розділ метрик ОС (ЛР4); для логів ЛР3 порожній."""
    if not (rep.tasks_runtime or rep.stack_tasks or rep.stack_free_min_words
            or rep.os_heap or rep.trace_summary):
        return []
    out: List[str] = ["### 5. Метрики ОС (ЛР4)", ""]
    if rep.tasks_runtime:
        total_rt = sum(t.runtime for t in rep.tasks_runtime.values())
        out += ["Завантаження за задачами (`TASKS`, run-time stats):", "",
                "| Задача | Пріоритет | Час, такти | Частка | Частка за тактами |",
                "| :--- | :---: | ---: | ---: | ---: |"]
        for t in sorted(rep.tasks_runtime.values(), key=lambda t: -t.runtime):
            share = f"{100.0 * t.runtime / total_rt:.2f}%" if total_rt else "—"
            pct = "<1%" if t.pct == 0.5 else f"{t.pct:g}%"
            out.append(f"| `{t.name}` | {t.prio if t.prio is not None else '—'} | {t.runtime} | {pct} | {share} |")
        if rep.cpu_from_idle_pct is not None:
            out.append("")
            out.append(f"* **CPU за IDLE (100 − IDLE):** `{rep.cpu_from_idle_pct}%`")
        out.append("")
    if rep.stack_tasks or rep.stack_free_min_words:
        out += ["Стек кожної задачі (пік від старту МК):", "",
                "| Задача | Зарезервовано, Б | Використано, Б | Вільно, Б | Запас |",
                "| :--- | ---: | ---: | ---: | ---: |"]
        for name, sp in sorted(rep.stack_tasks.items()):
            margin = f"{100.0 * sp.free / sp.reserved:.0f}%" if sp.reserved else "—"
            out.append(f"| `{name}` | {sp.reserved} | {sp.used} | {sp.free} | {margin} |")
        for name, w in sorted(rep.stack_free_min_words.items()):
            if name not in rep.stack_tasks:
                out.append(f"| `{name}` | — | — | {4 * w} (`free_min_words={w}`) | — |")
        res, used = stack_totals(rep)
        if res:
            out.append(f"| **разом задачі** | **{res}** | **{used}** | | |")
        out.append("")
    if rep.os_heap:
        h = rep.os_heap
        out.append(f"* **Heap ОС:** total = `{h.total}` Б, free = `{h.free}` Б, "
                   f"min_ever_free = `{h.min_ever_free}` Б → використано max = `{h.used_max}` Б")
        out.append("")
    if rep.trace_summary:
        n = len(rep.trace_events)
        out += [f"Хронологія задач (`TRACE`, {n} перемикань):", "",
                "| Задача | Відрізків | Час, мкс | Частка вікна | Найдовший відрізок, мкс |",
                "| :--- | ---: | ---: | ---: | ---: |"]
        for name, d in sorted(rep.trace_summary.items(), key=lambda kv: -kv[1]["total_us"]):
            out.append(f"| `{name}` | {int(d['slices'])} | {int(d['total_us'])} | {d['share_pct']}% | {int(d['max_slice_us'])} |")
        out.append("")
    return out


def stack_totals(rep: LogReport) -> Tuple[int, int]:
    """Сума зарезервованого й використаного стека задач (без MSP — стека переривань)."""
    tasks = {k: v for k, v in rep.stack_tasks.items() if k.upper() != "MSP"}
    return sum(v.reserved for v in tasks.values()), sum(v.used for v in tasks.values())


def render_side_by_side(reports: Dict[str, LogReport]) -> str:
    """Ключові метрики кількох логів поруч (напр. S3 ЛР3 і S3 ЛР4) — для порівняльної таблиці §7.3."""
    labels = list(reports)

    def pair(rep: LogReport, pid: str) -> str:
        ev = rep.event_latencies.get(pid)
        return f"{ev.max_us} / {ev.p99_us:.0f} мкс (n={ev.n})" if ev else "—"

    def jit(rep: LogReport, task: str) -> str:
        j = rep.jitter.get(task)
        return f"{j.max_us} мкс, missed {j.missed}" if j else "—"

    def load(rep: LogReport) -> str:
        if not rep.load_summary:
            return "—"
        ls = rep.load_summary
        return f"{ls['avg_pct']:.1f}% / {ls['max_pct']:.1f}%"

    def stack(rep: LogReport) -> str:
        if rep.stack_tasks:
            res, used = stack_totals(rep)
            msp = rep.stack_tasks.get("MSP")
            tail = f"; MSP {msp.used} / {msp.reserved} Б" if msp else ""
            return f"задачі {used} / {res} Б{tail}"
        if rep.stack_peak:
            return f"{rep.stack_peak.used} / {rep.stack_peak.reserved} Б"
        return "—"

    def heap(rep: LogReport) -> str:
        parts = []
        if rep.heap_peak:
            parts.append(f"newlib {rep.heap_peak.used} Б")
        if rep.os_heap:
            parts.append(f"ОС {rep.os_heap.used_max} / {rep.os_heap.total} Б")
        return ", ".join(parts) or "—"

    def counters(rep: LogReport) -> str:
        peak = rep.uart_errors.get("rx_peak")
        return (f"EVQ {rep.evq.get('dropped', '—')}, rx_overrun {rep.uart_errors.get('rx_overrun', '—')}"
                + (f" (rx_peak {peak} Б)" if peak is not None else "")
                + f", OVR {sum(rep.overruns.values()) if rep.overruns else 0}")

    rows = [
        ("Прошивка (`loop`, `git`)", lambda r: f"`{r.build.get('loop', '?')}` `{r.build.get('git', '?')}`"),
        ("Реакція B1, max / p99", lambda r: pair(r, "btn_b1_press->btn_b1_handled")),
        ("Реакція PIR, max / p99", lambda r: pair(r, "pir_trig->pir_handled")),
        ("Код: натискання → обробка, max / p99", lambda r: pair(r, "code_key_press->code_key_handled")),
        ("Команда UART, max / p99", lambda r: pair(r, "uart_cmd_rx->uart_cmd_tx")),
        ("Джитер `security`, max", lambda r: jit(r, "security")),
        ("Джитер `env`, max", lambda r: jit(r, "env")),
        ("Захоплення кадру, max", lambda r: pair(r, "capture_start->capture_done")),
        ("Запис кадру у Flash, max", lambda r: pair(r, "flash_wr_start->flash_wr_done")),
        ("CPU `LOAD pct`, avg / max", load),
        ("CPU за IDLE (ЛР4)", lambda r: f"{r.cpu_from_idle_pct}%" if r.cpu_from_idle_pct is not None else "—"),
        ("Стек: використано / зарезервовано", stack),
        ("Heap: використано", heap),
        ("Втрати: EVQ, UART, OVR", counters),
    ]
    out = ["## Порівняння логів поруч (`--log`)", "",
           "| Метрика | " + " | ".join(f"`{l}`" for l in labels) + " |",
           "| :--- | " + " | ".join(":---:" for _ in labels) + " |"]
    for title, fn in rows:
        out.append(f"| {title} | " + " | ".join(fn(reports[l]) for l in labels) + " |")
    # STAT за іменами задач: ЛР3 і ЛР4 мають різні імена (TASK_ALIASES_LAB3_TO_LAB4)
    names = sorted({n for r in reports.values() for n in r.stats})
    if names:
        out += ["", "`STAT` за іменами (max, мкс). Відповідність імен ЛР3 → ЛР4: "
                + "; ".join(f"`{k}` → " + (", ".join(f"`{x}`" for x in v) if v else "немає")
                            for k, v in TASK_ALIASES_LAB3_TO_LAB4.items()) + ".", "",
                "| `STAT name` | " + " | ".join(f"`{l}`" for l in labels) + " |",
                "| :--- | " + " | ".join(":---:" for _ in labels) + " |"]
        for n in names:
            out.append(f"| `{n}` | " + " | ".join(
                str(reports[l].stats[n].max_us) if n in reports[l].stats else "—" for l in labels) + " |")
    out.append("")
    return "\n".join(out)


def render_comparison_table(reports: Dict[str, LogReport]) -> str:
    """Формує зведену таблицю за шаблоном docs/metrics/metrics-spec.md §5."""
    s1 = reports.get("S1")
    s2 = reports.get("S2")
    s3 = reports.get("S3")

    def get_stat_max(rep: Optional[LogReport], key: str) -> str:
        if not rep or key not in rep.stats:
            return "—"
        return f"{rep.stats[key].max_us} мкс"

    def get_b1_max(rep: Optional[LogReport]) -> str:
        if not rep:
            return "—"
        ev = rep.event_latencies.get("btn_b1_press->btn_b1_handled")
        if ev:
            return f"{ev.max_us} мкс (p99: {ev.p99_us:.0f})"
        if "btn_b1_lat" in rep.stats:
            return f"{rep.stats['btn_b1_lat'].max_us} мкс"
        return "—"

    def get_pir_max(rep: Optional[LogReport]) -> str:
        if not rep:
            return "—"
        ev = rep.event_latencies.get("pir_trig->pir_handled")
        if ev:
            return f"{ev.max_us} мкс (p99: {ev.p99_us:.0f})"
        if "pir_lat" in rep.stats:
            return f"{rep.stats['pir_lat'].max_us} мкс"
        return "—"

    def get_jit_max(rep: Optional[LogReport], task: str) -> str:
        if not rep or task not in rep.jitter:
            return "—"
        j = rep.jitter[task]
        return f"{j.max_us} мкс (missed: {j.missed})"

    def get_cam_max(rep: Optional[LogReport]) -> str:
        if not rep:
            return "—"
        ev = rep.event_latencies.get("capture_start->capture_done")
        if ev:
            return f"{ev.max_us} мкс"
        for k in ("camera", "cam_test"):
            if k in rep.stats:
                return f"{rep.stats[k].max_us} мкс"
        return "—"

    def get_flash_max(rep: Optional[LogReport]) -> str:
        if not rep:
            return "—"
        ev = rep.event_latencies.get("flash_wr_start->flash_wr_done")
        if ev:
            return f"{ev.max_us} мкс"
        if "storage" in rep.stats:
            return f"{rep.stats['storage'].max_us} мкс"
        return "—"

    def get_cpu(rep: Optional[LogReport]) -> str:
        if not rep or not rep.load_summary:
            return "—"
        ls = rep.load_summary
        return f"{ls['avg_pct']:.1f}% / {ls['max_pct']:.1f}%"

    def get_stack(rep: Optional[LogReport]) -> str:
        if not rep or not rep.stack_peak:
            return "—"
        sp = rep.stack_peak
        return f"{sp.used} / {sp.reserved} Б (free {sp.free} Б)"

    rows = [
        ("Період головного циклу $T_{\\text{loop}}$, max", get_stat_max(s1, "loop"), get_stat_max(s2, "loop"), get_stat_max(s3, "loop"), "—", "DWT", "—"),
        ("Затримка реакції на кнопку B1 (`PC13`), max", get_b1_max(s1), get_b1_max(s2), get_b1_max(s3), "TBD (PRD)", "DWT + PulseView", "—"),
        ("Затримка реакції на PIR (`PA0`), max", get_pir_max(s1), get_pir_max(s2), get_pir_max(s3), "TBD (PRD)", "DWT + PulseView", "—"),
        ("Джитер задачі `security`, max", get_jit_max(s1, "security"), get_jit_max(s2, "security"), get_jit_max(s3, "security"), "TBD (PRD)", "DWT", "—"),
        ("Час захоплення кадру OV7670", get_cam_max(s1), get_cam_max(s2), get_cam_max(s3), "0.53–1.07 с (теор.)", "DWT + PulseView", "—"),
        ("Час запису/стирання W25Q64", get_flash_max(s1), get_flash_max(s2), get_flash_max(s3), "0.5–400 мс / 0.66 с", "DWT", "—"),
        ("Завантаження CPU, avg / max", get_cpu(s1), get_cpu(s2), get_cpu(s3), "< 100%", "DWT (`LOAD`)", "—"),
        ("Глибина стека (Painted)", get_stack(s1), get_stack(s2), get_stack(s3), "Запас > 20%", "Stack Painting", "—"),
    ]

    out = [
        "## Зведена таблиця результатів S1–S3 (шаблон `metrics-spec.md` §5)",
        "",
        "| Метрика | S1 (Простій) | S2 (Номінал) | S3 (Пік, max) | Вимога PRD / Теорія | Спосіб виміру | Вердикт |",
        "| :--- | :---: | :---: | :---: | :---: | :---: | :---: |",
    ]
    for r in rows:
        out.append(f"| {r[0]} | {r[1]} | {r[2]} | {r[3]} | {r[4]} | {r[5]} | {r[6]} |")
    out.append("")
    return "\n".join(out)


def write_csv(reports: List[LogReport], csv_path: Path) -> None:
    csv_path.parent.mkdir(parents=True, exist_ok=True)
    with csv_path.open("w", newline="", encoding="utf-8") as f:
        writer = csv.writer(f)
        writer.writerow(
            ["scenario", "git", "category", "name", "n", "min_us", "avg_us", "max_us", "p99_us", "extra"]
        )
        for rep in reports:
            git_hash = rep.build.get("git", rep.stats_meta.get("git", "unknown"))
            for st in rep.stats.values():
                writer.writerow(
                    [rep.scenario, git_hash, "STAT", st.name, st.n, st.min_us, st.avg_us, st.max_us, "", ""]
                )
            for ev in rep.event_latencies.values():
                writer.writerow(
                    [rep.scenario, git_hash, "EVT_PAIR", ev.pair_id, ev.n, ev.min_us, ev.avg_us, ev.max_us, ev.p99_us, ""]
                )
            for jit in rep.jitter.values():
                writer.writerow(
                    [rep.scenario, git_hash, "JITTER", jit.name, "", jit.min_us, "", jit.max_us, "", f"period={jit.period_us};missed={jit.missed}"]
                )
            if rep.load_summary:
                ls = rep.load_summary
                writer.writerow(
                    [rep.scenario, git_hash, "LOAD_PCT", "cpu_load", int(ls["n"]), ls["min_pct"], ls["avg_pct"], ls["max_pct"], "", "unit=%"]
                )
            if rep.stack_peak:
                sp = rep.stack_peak
                writer.writerow(
                    [rep.scenario, git_hash, "STACK_B", "stack", "", "", "", sp.used, "", f"reserved={sp.reserved};free={sp.free}"]
                )
            for name, sp in rep.stack_tasks.items():
                writer.writerow(
                    [rep.scenario, git_hash, "STACK_TASK_B", name, "", "", "", sp.used, "", f"reserved={sp.reserved};free={sp.free}"]
                )
            if rep.os_heap:
                h = rep.os_heap
                writer.writerow(
                    [rep.scenario, git_hash, "OSHEAP_B", "os_heap", "", "", "", h.used_max, "", f"total={h.total};min_ever_free={h.min_ever_free}"]
                )
            for t in rep.tasks_runtime.values():
                writer.writerow(
                    [rep.scenario, git_hash, "TASK_PCT", t.name, "", "", t.pct, "", "", f"runtime={t.runtime};prio={t.prio}"]
                )
            if rep.cpu_from_idle_pct is not None:
                writer.writerow(
                    [rep.scenario, git_hash, "CPU_IDLE_PCT", "cpu_from_idle", "", "", rep.cpu_from_idle_pct, "", "", "unit=%"]
                )


def parse_extra_pair(spec: str) -> Tuple[str, str, str]:
    if ":" not in spec:
        raise argparse.ArgumentTypeError("Формат пари подій має бути start_event:end_event")
    start_ev, end_ev = spec.split(":", 1)
    return (start_ev.strip(), end_ev.strip(), f"Custom pair {start_ev} -> {end_ev}")


def parse_labeled_log(spec: str) -> Tuple[str, Path]:
    if "=" not in spec:
        raise argparse.ArgumentTypeError("Формат: МІТКА=ФАЙЛ, напр. S3_lab4=docs/metrics/lab4/S3_uart_raw.txt")
    label, path = spec.split("=", 1)
    return label.strip(), Path(path.strip())


def main(argv: Optional[List[str]] = None) -> int:
    parser = argparse.ArgumentParser(
        description="Парсер UART EVT/STAT-логів SmartLock (ЛР3, задача M03)"
    )
    parser.add_argument("log_file", nargs="?", type=Path, help="Шлях до одиночного лог-файлу UART")
    parser.add_argument("--s1", type=Path, help="Лог-файл сценарію S1 (Простій)")
    parser.add_argument("--s2", type=Path, help="Лог-файл сценарію S2 (Номінальний режим)")
    parser.add_argument("--s3", type=Path, help="Лог-файл сценарію S3 (Найгірший випадок)")
    parser.add_argument(
        "--pair",
        action="append",
        type=parse_extra_pair,
        default=[],
        help="Додаткова пара подій start_event:end_event (можна вказати кілька разів)",
    )
    parser.add_argument(
        "--log",
        action="append",
        type=parse_labeled_log,
        default=[],
        metavar="МІТКА=ФАЙЛ",
        help="Лог з довільною міткою, напр. S3_lab3=… і S3_lab4=…: ключові метрики виводяться поруч "
        "(можна вказати кілька разів; разом із --s1/--s2/--s3 теж)",
    )
    parser.add_argument("--md", type=Path, help="Зберегти Markdown-звіт у вказаний файл")
    parser.add_argument("--json", type=Path, help="Зберегти повний JSON-дамп метрик у вказаний файл")
    parser.add_argument("--csv", type=Path, help="Зберегти зведену CSV-таблицю у вказаний файл")

    args = parser.parse_args(argv)

    inputs: List[Tuple[str, Path]] = []
    if args.log_file:
        inputs.append(("RUN", args.log_file))
    if args.s1:
        inputs.append(("S1", args.s1))
    if args.s2:
        inputs.append(("S2", args.s2))
    if args.s3:
        inputs.append(("S3", args.s3))
    for label, path in args.log:
        if label in {name for name, _ in inputs}:
            parser.error(f"Мітка '{label}' повторюється.")
        inputs.append((label, path))

    if not inputs:
        parser.error("Вкажіть лог-файл або хоча б один параметр --s1 / --s2 / --s3 / --log.")

    reports_by_name: Dict[str, LogReport] = {}
    for scen, fpath in inputs:
        if not fpath.exists():
            print(f"Помилка: файл '{fpath}' не знайдено.", file=sys.stderr)
            return 2
        reports_by_name[scen] = parse_log_file(fpath, scenario=scen, extra_pairs=args.pair)

    md_parts: List[str] = []
    if any(k in reports_by_name for k in ("S1", "S2", "S3")):
        md_parts.append(render_comparison_table(reports_by_name))
    if args.log:
        md_parts.append(render_side_by_side(reports_by_name))
    for rep in reports_by_name.values():
        md_parts.append(render_single_markdown(rep))

    full_md = "\n".join(md_parts)
    print(full_md)

    if args.md:
        args.md.parent.mkdir(parents=True, exist_ok=True)
        args.md.write_text(full_md, encoding="utf-8")

    if args.json:
        args.json.parent.mkdir(parents=True, exist_ok=True)
        payload = {k: asdict(v) for k, v in reports_by_name.items()}
        args.json.write_text(json.dumps(payload, ensure_ascii=False, indent=2), encoding="utf-8")

    if args.csv:
        write_csv(list(reports_by_name.values()), args.csv)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())