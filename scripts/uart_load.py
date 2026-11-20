#!/usr/bin/env python3
"""
Генератор UART-навантаження та збирач логів SmartLock (ЛР3, задача M03).

Призначений для сценарію S3 (найгірший випадок) та стрес-тесту потоку команд
за методичкою §6.2.3. Підтримує роботу з реальним COM-портом (pyserial),
а також автономну перевірку без плати у режимах --dry-run та --mock.

Узгоджено з прошивкою:
- firmware/App/cmd.c (STATS, STATS RESET, MEM, PING <n>, MARK <n>, CAM TEST)
- docs/metrics/metrics-spec.md (§3, §4 S3)
"""

from __future__ import annotations

import argparse
import re
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import List, Optional, Protocol

try:
    import serial  # type: ignore
except ImportError:
    serial = None  # Дозволяє запускати --dry-run та --mock без встановленого pyserial

RE_PONG = re.compile(r"^PONG seq=(?P<seq>\d+) t_us=(?P<t_us>\d+)$")


class SerialTransport(Protocol):
    """Мінімальний інтерфейс послідовного порту для реального UART і Mock-стенда."""

    def write(self, data: bytes) -> int:
        ...

    def flush(self) -> None:
        ...

    def readline(self) -> bytes:
        ...

    def close(self) -> None:
        ...


class MockSmartLockSerial:
    """
    Імітатор UART-відповідей прошивки SmartLock (STM32F411, 96 МГц) для тестування
    генератора навантаження без підключеної плати.
    """

    def __init__(self, git_hash: str = "mock001", drop_every: int = 0) -> None:
        self.git_hash = git_hash
        self.drop_every = drop_every
        self._rx_buf = bytearray()
        self._out_lines: List[bytes] = []
        self._t_us = 1_000_000
        self._loops = 12_500
        self._ping_seen = 0
        self._cmds_total = 0
        self._last_load_step = 0
        self._push_line(
            f"BUILD,fw=SmartLock,lab=LR3,git={self.git_hash},cpu_hz=96000000,loop=superloop"
        )

    def _push_line(self, text: str) -> None:
        self._out_lines.append((text + "\r\n").encode("utf-8"))

    def _advance_time(self, delta_us: int) -> None:
        self._t_us += delta_us
        self._loops += max(1, delta_us // 70)
        if (self._t_us // 1_000_000) > self._last_load_step:
            self._last_load_step = self._t_us // 1_000_000
            pct = min(98, 18 + (self._cmds_total % 45))
            self._push_line(f"LOAD pct={pct}")

    def _handle_command(self, cmd: str) -> None:
        self._cmds_total += 1
        if cmd == "STATS RESET":
            self._advance_time(250)
            self._push_line(f"EVT t_us={self._t_us} name=stats_reset")
            self._push_line("STATS,reset")
            return

        if cmd.startswith("PING "):
            seq_str = cmd[5:].strip()
            self._ping_seen += 1
            if self.drop_every > 0 and (self._ping_seen % self.drop_every == 0):
                # Імітуємо втрату байта RX при перевантаженні UART
                self._advance_time(500)
                return
            t_rx = self._t_us + 120
            t_tx = t_rx + 340
            self._advance_time(460)
            self._push_line(f"EVT t_us={t_rx} name=uart_cmd_rx")
            self._push_line(f"PONG seq={seq_str} t_us={t_rx + 80}")
            self._push_line(f"EVT t_us={t_tx} name=uart_cmd_tx")
            return

        if cmd.startswith("MARK "):
            arg = cmd[5:].strip()
            self._advance_time(180)
            self._push_line(f"EVT t_us={self._t_us} name=mark_{arg}")
            return

        if cmd == "MEM":
            self._advance_time(420)
            self._push_line("STACK reserved=2048 used=1416 free=632")
            self._push_line("HEAP arena=1024 used=256 free=768")
            return

        if cmd == "STATS":
            self._advance_time(120_000)
            dropped = self._ping_seen // self.drop_every if self.drop_every > 0 else 0
            self._push_line(
                f"STATS_BEGIN git={self.git_hash} t_us={self._t_us} loops={self._loops}"
            )
            self._push_line(
                f"STAT name=loop n={self._loops} min_us=45 avg_us=82 max_us=34200"
            )
            self._push_line("CPU name=cpu_load windows=5 min_pm=180 avg_pm=420 max_pm=790")
            self._push_line("STAT name=fsm n=24 min_us=14 avg_us=25 max_us=58")
            self._push_line(f"POLL name=fsm polls={self._loops} runs=24")
            self._push_line("STAT name=buttons n=500 min_us=18 avg_us=26 max_us=64")
            self._push_line("JITTER name=buttons period_us=10000 min_us=-10 max_us=24100 missed=2")
            self._push_line(
                "JHIST name=buttons n=499 h_lt100us=480 h_lt1ms=15 h_lt10ms=2 h_lt100ms=2 h_ge100ms=0"
            )
            self._push_line("STAT name=security n=250 min_us=210 avg_us=260 max_us=1850")
            self._push_line("JITTER name=security period_us=20000 min_us=-15 max_us=14200 missed=0")
            self._push_line(
                "JHIST name=security n=249 h_lt100us=235 h_lt1ms=12 h_lt10ms=2 h_lt100ms=0 h_ge100ms=0"
            )
            self._push_line(
                f"STAT name=uart n={self._cmds_total} min_us=190 avg_us=480 max_us=4100"
            )
            self._push_line(f"POLL name=uart polls={self._loops} runs={self._cmds_total}")
            self._push_line("STAT name=btn_b1_lat n=5 min_us=380 avg_us=1120 max_us=4900")
            self._push_line("OVR name=btn_b1 count=0")
            self._push_line("STAT name=evq_wait n=5 min_us=75 avg_us=120 max_us=210")
            self._push_line("EVQ len=8 pending=0 max_depth=2 posted=5 dropped=0")
            self._push_line(f"UART rx_overrun={dropped} cmd_overflow=0")
            self._push_line("EVTLOG pending=0 dropped=0")
            self._push_line("STACK reserved=2048 used=1416 free=632")
            self._push_line("HEAP arena=1024 used=256 free=768")
            self._push_line("STATS_END")
            return

        self._push_line(f"# unknown command '{cmd}', HELP for list")

    def write(self, data: bytes) -> int:
        self._rx_buf.extend(data)
        while b"\n" in self._rx_buf:
            raw_line, _, rest = self._rx_buf.partition(b"\n")
            self._rx_buf = bytearray(rest)
            cmd = raw_line.decode("utf-8", errors="replace").strip("\r\n ")
            if cmd:
                self._handle_command(cmd)
        return len(data)

    def flush(self) -> None:
        pass

    def readline(self) -> bytes:
        if self._out_lines:
            return self._out_lines.pop(0)
        return b""

    def close(self) -> None:
        self._out_lines.clear()


@dataclass
class LoadRunResult:
    mode: str
    port: str
    baudrate: int
    profile: str
    commands_sent: int = 0
    pings_sent: int = 0
    pongs_received: int = 0
    lines_received: int = 0
    elapsed_s: float = 0.0
    sent_preview: List[str] = field(default_factory=list)
    captured_lines: List[str] = field(default_factory=list)

    @property
    def lost_pings(self) -> int:
        return max(0, self.pings_sent - self.pongs_received)

    @property
    def effective_rate_hz(self) -> float:
        if self.elapsed_s <= 0.0:
            return 0.0
        return round(self.commands_sent / self.elapsed_s, 2)


def build_command_for_step(step: int, profile: str) -> str:
    """Формує чергову команду UART згідно з обраним профілем навантаження."""
    if profile == "mixed":
        if step % 25 == 0:
            return f"MARK {step}"
        if step % 10 == 0:
            return "MEM"
    return f"PING {step}"


def drain_incoming(
    transport: SerialTransport,
    result: LoadRunResult,
    seen_pongs: set[int],
    max_reads: int = 256,
) -> None:
    """Вичитує всі доступні рядки з буфера прийому UART без блокування."""
    for _ in range(max_reads):
        raw = transport.readline()
        if not raw:
            break
        line = raw.decode("utf-8", errors="replace").rstrip("\r\n")
        if not line:
            continue
        result.lines_received += 1
        result.captured_lines.append(line)
        m = RE_PONG.match(line)
        if m:
            seen_pongs.add(int(m.group("seq")))


def run_uart_load(
    port: Optional[str],
    baudrate: int = 115200,
    duration_s: float = 10.0,
    count: Optional[int] = None,
    rate_hz: float = 20.0,
    burst: bool = False,
    profile: str = "ping",
    start_reset: bool = True,
    final_stats: bool = True,
    tail_wait_s: float = 0.35,
    dry_run: bool = False,
    mock: bool = False,
    mock_drop_every: int = 0,
    out_file: Optional[Path] = None,
) -> LoadRunResult:
    mode = "dry-run" if dry_run else ("mock" if mock else "serial")
    port_label = port or ("MOCK_UART" if mock else "DRY_RUN")
    interval_s = 0.0 if burst else (1.0 / rate_hz if rate_hz > 0 else 0.0)

    result = LoadRunResult(
        mode=mode,
        port=port_label,
        baudrate=baudrate,
        profile=profile,
    )

    if dry_run:
        planned_count = (
            count
            if count is not None
            else max(1, int(round(duration_s * (100.0 if burst else rate_hz))))
        )
        if start_reset:
            result.sent_preview.append("STATS RESET")
            result.commands_sent += 1
        for seq in range(1, planned_count + 1):
            cmd = build_command_for_step(seq, profile)
            if cmd.startswith("PING "):
                result.pings_sent += 1
            result.commands_sent += 1
            if len(result.sent_preview) < 12:
                result.sent_preview.append(cmd)
        if final_stats:
            result.sent_preview.append("STATS")
            result.commands_sent += 1
        result.elapsed_s = 0.0 if burst else round(planned_count * interval_s, 3)
        return result

    transport: SerialTransport
    if mock:
        transport = MockSmartLockSerial(drop_every=mock_drop_every)
    else:
        if not port:
            raise ValueError("Для роботи з платою вкажіть --port (наприклад, COM3 або /dev/ttyACM0).")
        if serial is None:
            raise RuntimeError(
                "Бібліотека 'pyserial' не встановлена. Встановіть її командою: pip install pyserial"
            )
        transport = serial.Serial(port=port, baudrate=baudrate, timeout=0.02, write_timeout=0.5)

    seen_pongs: set[int] = set()
    t_start = time.monotonic()

    try:
        drain_incoming(transport, result, seen_pongs)

        if start_reset:
            transport.write(b"STATS RESET\r\n")
            transport.flush()
            result.commands_sent += 1
            result.sent_preview.append("STATS RESET")
            if not mock:
                time.sleep(0.03)
            drain_incoming(transport, result, seen_pongs)

        seq = 0
        next_tick = time.monotonic()
        deadline = t_start + duration_s

        while True:
            if count is not None:
                if seq >= count:
                    break
            else:
                if time.monotonic() >= deadline:
                    break

            seq += 1
            cmd = build_command_for_step(seq, profile)
            transport.write((cmd + "\r\n").encode("ascii"))
            transport.flush()
            result.commands_sent += 1
            if cmd.startswith("PING "):
                result.pings_sent += 1
            if len(result.sent_preview) < 12:
                result.sent_preview.append(cmd)

            drain_incoming(transport, result, seen_pongs)

            if not burst and interval_s > 0.0 and not mock:
                next_tick += interval_s
                sleep_for = next_tick - time.monotonic()
                if sleep_for > 0:
                    time.sleep(sleep_for)

        if final_stats:
            transport.write(b"STATS\r\n")
            transport.flush()
            result.commands_sent += 1
            result.sent_preview.append("STATS")

        end_wait = time.monotonic() + (0.01 if mock else tail_wait_s)
        while True:
            drain_incoming(transport, result, seen_pongs)
            if mock or time.monotonic() >= end_wait:
                break
            time.sleep(0.01)

    finally:
        transport.close()

    result.elapsed_s = round(max(1e-6, time.monotonic() - t_start), 4)
    result.pongs_received = len(seen_pongs)

    if out_file is not None:
        out_file.parent.mkdir(parents=True, exist_ok=True)
        out_file.write_text("\n".join(result.captured_lines) + "\n", encoding="utf-8")

    return result


def format_summary(res: LoadRunResult, out_file: Optional[Path] = None) -> str:
    lines = [
        f"=== Результат UART-навантаження ({res.mode}) ===",
        f"Порт / швидкість    : {res.port} @ {res.baudrate} 8N1",
        f"Профіль команд      : {res.profile}",
        f"Відправлено команд  : {res.commands_sent} (з них PING: {res.pings_sent})",
        f"Отримано PONG       : {res.pongs_received} (втрачено PING: {res.lost_pings})",
        f"Прийнято рядків логу: {res.lines_received}",
        f"Тривалість / темп   : {res.elapsed_s:.3f} с ({res.effective_rate_hz:.1f} команд/с)",
        f"Приклад команд      : {', '.join(res.sent_preview)}",
    ]
    if out_file is not None:
        lines.append(f"Збережений лог      : {out_file}")
    return "\n".join(lines)


def main(argv: Optional[List[str]] = None) -> int:
    parser = argparse.ArgumentParser(
        description="Генератор UART-навантаження SmartLock для сценарію S3 (ЛР3, M03)"
    )
    parser.add_argument("--port", type=str, help="COM-порт плати (напр. COM3 або /dev/ttyACM0)")
    parser.add_argument("--baud", type=int, default=115200, help="Швидкість UART (за замовчуванням 115200)")
    parser.add_argument("--duration", type=float, default=10.0, help="Тривалість генерації навантаження, с (за замовчуванням 10)")
    parser.add_argument("--count", type=int, default=None, help="Фіксована кількість команд замість --duration")
    parser.add_argument("--rate", type=float, default=20.0, help="Частота відправки команд, Гц (за замовчуванням 20 Гц)")
    parser.add_argument("--burst", action="store_true", help="Стрес-режим без пауз між командами (§6.2.3)")
    parser.add_argument(
        "--profile",
        choices=["ping", "mixed"],
        default="mixed",
        help="Набір команд: ping (лише PING) або mixed (PING + періодичні MEM і MARK)",
    )
    parser.add_argument("--no-start-reset", action="store_true", help="Не надсилати STATS RESET на старті")
    parser.add_argument("--no-final-stats", action="store_true", help="Не запитувати фінальний блок STATS у кінці")
    parser.add_argument("--tail-wait", type=float, default=0.35, help="Очікування відповіді STATS у кінці, с")
    parser.add_argument("--dry-run", action="store_true", help="Перевірка плану команд без відкриття COM-порту")
    parser.add_argument("--mock", action="store_true", help="Запуск на вбудованому імітаторі плати без заліза")
    parser.add_argument("--mock-drop-every", type=int, default=0, help="Імітувати втрату кожного N-го PING у --mock")
    parser.add_argument("--out", type=Path, help="Шлях для збереження прийнятого UART-логу (.log/.txt)")

    args = parser.parse_args(argv)

    try:
        res = run_uart_load(
            port=args.port,
            baudrate=args.baud,
            duration_s=args.duration,
            count=args.count,
            rate_hz=args.rate,
            burst=args.burst,
            profile=args.profile,
            start_reset=not args.no_start_reset,
            final_stats=not args.no_final_stats,
            tail_wait_s=args.tail_wait,
            dry_run=args.dry_run,
            mock=args.mock,
            mock_drop_every=args.mock_drop_every,
            out_file=args.out,
        )
    except (ValueError, RuntimeError, OSError) as exc:
        print(f"Помилка: {exc}", file=sys.stderr)
        return 2

    print(format_summary(res, out_file=args.out))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

    