#!/usr/bin/env python3
"""
Парсер та аналізатор діагностичних UART-логів SmartLock (ЛР3, задача M03).

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
    r"^STATS_BEGIN git=(?P<git>\S+) t_us=(?P<t_us>\d+) loops=(?P<loops>\d+)$"
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
RE_UART = re.compile(
    r"^UART rx_overrun=(?P<rx_overrun>\d+) cmd_overflow=(?P<cmd_overflow>\d+)$"
)
RE_EVTLOG = re.compile(r"^EVTLOG pending=(?P<pending>\d+) dropped=(?P<dropped>\d+)$")
RE_LOAD = re.compile(r"^LOAD pct=(?P<pct>\d+)$")
RE_EVT = re.compile(r"^EVT t_us=(?P<t_us>\d+) name=(?P<name>\S+)$")
RE_STACK = re.compile(
    r"^STACK reserved=(?P<reserved>\d+) used=(?P<used>\d+) free=(?P<free>\d+)$"
)
RE_HEAP = re.compile(
    r"^HEAP arena=(?P<arena>\d+) used=(?P<used>\d+) free=(?P<free>\d+)$"
)


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
            report.uart_errors = {k: int(v) for k, v in m.groupdict().items()}
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
        f"`cpu_hz={rep.cpu_hz}`",
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

    return "\n".join(lines)


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


def parse_extra_pair(spec: str) -> Tuple[str, str, str]:
    if ":" not in spec:
        raise argparse.ArgumentTypeError("Формат пари подій має бути start_event:end_event")
    start_ev, end_ev = spec.split(":", 1)
    return (start_ev.strip(), end_ev.strip(), f"Custom pair {start_ev} -> {end_ev}")


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

    if not inputs:
        parser.error("Вкажіть лог-файл або хоча б один параметр --s1 / --s2 / --s3.")

    reports_by_name: Dict[str, LogReport] = {}
    for scen, fpath in inputs:
        if not fpath.exists():
            print(f"Помилка: файл '{fpath}' не знайдено.", file=sys.stderr)
            return 2
        reports_by_name[scen] = parse_log_file(fpath, scenario=scen, extra_pairs=args.pair)

    md_parts: List[str] = []
    if any(k in reports_by_name for k in ("S1", "S2", "S3")):
        md_parts.append(render_comparison_table(reports_by_name))
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