#!/usr/bin/env python3
"""
Запис UART-логу стендового прогону SmartLock (S1, S2, S3, E2E) без pyserial.

Що робить:
- відкриває порт ST-LINK VCP (115200 8N1 за замовчуванням) через termios;
- пише сирий потік байт без змін у <name>_uart_raw.txt — саме цей файл
  читає scripts/parse_uart_logs.py;
- паралельно пише <name>_uart_ts.txt: кожен рядок із часом ПК і зміщенням від
  старту, а також надіслані команди й нотатки оператора;
- рядки, введені з клавіатури, надсилає на плату (рядкова команда, напр.
  STATS, — з "\r\n"; один символ — без завершення, як стендова команда);
  "#текст" — лише нотатка в ts-файл; "!" або "!текст" — MARK з наступним
  номером (подія в EVT-лозі плати + нотатка з описом дії);
- за --duration зупиняється сам, наприкінці надсилає --end (типово STATS, MEM);
- для STATS, MEM, PING n, MARK n чекає відповіді й повторює команду: прийом UART
  у прошивці опитувальний, команда під час блокувальної задачі губиться цілком;
  команди без відповіді після всіх спроб потрапляють у conditions;
- для S1/S2/S3 друкує нагадування з протоколу docs/lab_3/test-scenarios.md;
- зберігає <name>_conditions.md: git-хеш з рядка BUILD, час, порт, команди,
  MARK і поля для умов, які оператор доповнює вручну.

Приклад (S2, 60 хв після переходу в LOCKED):
    python3 scripts/uart_capture.py --port /dev/cu.usbmodem12303 \
        --name S2 --out docs/metrics --duration 62 --plan S2
Приклад (S3: запис і навантаження UART одним процесом — порт плати один):
    python3 scripts/uart_capture.py --port /dev/cu.usbmodem12303 --name S3 --plan S3 \
        --load-rate 20 --load-profile mixed --char-gap 0 --retries 1
"""

from __future__ import annotations

import argparse
import datetime as dt
import os
import re
import select
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import List, Optional, Tuple

try:
    import termios  # лише Linux / macOS; на Windows — через WSL
except ImportError:  # pragma: no cover - залежить від ОС
    termios = None  # type: ignore[assignment]

BAUD_RATES = (9600, 19200, 38400, 57600, 115200)
# Константи швидкості termios; на Windows термінала немає, а скрипт імпортується лише заради
# чистих функцій (тести, uart_load), тож значення не потрібні
BAUDS = {b: (getattr(termios, f"B{b}") if termios else None) for b in BAUD_RATES}

RE_BUILD = re.compile(r"^BUILD,.*\bgit=(?P<git>[^,\s]+)")

# Нагадування з протоколів S1–S3 (docs/lab_3/test-scenarios.md), хвилини від старту запису.
PLANS = {
    "S1": [
        (0, "S1: не торкатися стенда і не надсилати команд 10 хв"),
        (10, "S1: 10 хв минуло — можна завершувати (Ctrl+C), наприкінці буде STATS/MEM"),
    ],
    "S2": [
        (0, "S2: штатне опитування без зовнішніх подій (00–10 хв)"),
        (10, "S2: один контрольований сигнал PIR у LOCKED (введіть ! з описом)"),
        (20, "S2: одна подія TAMPER від MPU-6050 за погодженим порогом"),
        (30, "S2: правильний код -> UNLOCKED; не повинно бути автоблокування >= 5 хв"),
        (40, "S2: заблокувати через USER B1, після LOCKED один неправильний код"),
        (50, "S2: штатна робота без подій до кінця прогону (>= 60 хв)"),
        (60, "S2: 60 хв минуло — можна завершувати"),
    ],
    "S3": [
        (0, "S3: запустити PulseView; камера/Flash/потік UART; 30 контрольованих подій"),
        (10, "S3: 10 хв минуло — завершувати, коли набрано 30 валідних подій"),
    ],
}


@dataclass
class Capture:
    """Стан одного запису: розбиття потоку на рядки і журнал дій оператора."""

    t0: float
    git: Optional[str] = None
    build_line: Optional[str] = None
    lines: int = 0
    rx_bytes: int = 0
    marks: List[Tuple[float, int, str]] = field(default_factory=list)
    sent: List[Tuple[float, str]] = field(default_factory=list)
    unanswered: List[Tuple[float, str]] = field(default_factory=list)
    notes: List[Tuple[float, str]] = field(default_factory=list)
    load_sent: int = 0          # команд навантаження (--load-rate / --load-burst)
    load_pongs: int = 0         # PONG у відповідь на PING навантаження
    load_first: Optional[float] = None
    load_last: Optional[float] = None
    load_chunks: int = 0        # пауз між пакетами (--load-chunk)
    _partial: bytes = b""

    def feed(self, data: bytes) -> List[str]:
        """Приймає сирі байти, повертає завершені рядки без \\r\\n."""
        self.rx_bytes += len(data)
        buf = self._partial + data
        parts = re.split(rb"\r\n|\n|\r", buf)
        self._partial = parts.pop()
        out = []
        for raw in parts:
            if not raw:
                continue
            line = raw.decode("utf-8", errors="replace")
            self.lines += 1
            m = RE_BUILD.match(line)
            if m and self.git is None:
                self.git = m.group("git")
                self.build_line = line
            if line.startswith("PONG seq="):
                self.load_pongs += 1
            out.append(line)
        return out

    def flush(self) -> List[str]:
        """Повертає незавершений залишок (якщо плата не дописала рядок)."""
        rest, self._partial = self._partial, b""
        return self.feed(rest + b"\n") if rest else []


def stamp(t: float, t0: float) -> str:
    wall = dt.datetime.fromtimestamp(t).isoformat(timespec="milliseconds")
    return f"{wall} +{t - t0:9.3f}s"


def expected_reply(cmd: str) -> Optional["re.Pattern[str]"]:
    """
    Рядок, за яким видно, що плата прийняла команду (формат — App/cmd.c, evt.c, stats.c).
    Прийом UART у прошивці опитувальний, без буфера: команда, що прийшла під час
    блокувальної задачі, губиться цілком, тому службові команди перевіряються і повторюються.
    """
    if cmd in ("STATS", "STATS RAW"):
        return re.compile(r"^STATS_END$")
    if cmd == "STATS RESET":
        return re.compile(r"^STATS,reset$")
    if cmd == "MEM":
        return re.compile(r"^HEAP ")
    m = re.fullmatch(r"PING (\d+)", cmd)
    if m:
        return re.compile(rf"^PONG seq={int(m.group(1))}\b")
    m = re.fullmatch(r"MARK (\d+)", cmd)
    if m:
        return re.compile(rf"\bname=mark_{int(m.group(1))}$")
    return None


def load_command(step: int, profile: str) -> str:
    """
    Чергова команда навантаження UART для S3 — ті самі профілі, що в scripts/uart_load.py
    (build_command_for_step): ping — лише PING <n>; mixed — кожна 25-та MARK <n>, кожна 10-та MEM.
    Порт плати один, тож у S3 навантаження генерує той самий процес, що пише лог.
    """
    if profile == "mixed":
        if step % 25 == 0:
            return f"MARK {step}"
        if step % 10 == 0:
            return "MEM"
    return f"PING {step}"


# ST-LINK VCP зависає на безперервному потоці з ПК довшому за ~380 Б, якщо плата
# одночасно відповідає (docs/lab_3/bench-runs.md, «Відомі обмеження»; лог 22).
STLINK_SAFE_BYTES = 380


def chunk_bytes(first_step: int, count: int, profile: str) -> int:
    """Байтів у пакеті з count команд навантаження, починаючи з кроку first_step."""
    return sum(len(encode_command(load_command(first_step + i, profile))) for i in range(count))


def pace_after_load(in_chunk: int, chunk: int, gap: float, now: float,
                    next_load: float) -> Tuple[int, float, bool]:
    """
    Пакетна подача навантаження (--load-chunk): після chunk команд — пауза gap с,
    поки плата відповідає і буфер ST-LINK спорожнюється. Повертає (команд у поточному
    пакеті, час наступної команди, чи щойно закрито пакет). chunk = 0 — без пакетів.
    """
    if chunk <= 0:
        return in_chunk, next_load, False
    in_chunk += 1
    if in_chunk >= chunk:
        return 0, max(next_load, now + gap), True
    return in_chunk, next_load, False


def encode_command(text: str) -> bytes:
    """
    Один символ — стендова команда без завершення; інше — рядкова команда з \\r\\n.
    Провідний \\r завершує обрізаний залишок попередньої команди (якщо її \\r загубився),
    інакше наступна склеїться з ним ('MMARK 2'); порожній \\r прошивка ігнорує.
    """
    if len(text) == 1:
        return text.encode("ascii")
    return b"\r" + text.encode("ascii") + b"\r\n"


def parse_operator(text: str, next_mark: int) -> Tuple[str, Optional[str], str]:
    """
    Розбирає рядок оператора.
    Повертає (kind, command, note): kind = "note" | "mark" | "send" | "skip".
    """
    text = text.strip()
    if not text:
        return "skip", None, ""
    if text.startswith("#"):
        return "note", None, text[1:].strip()
    if text.startswith("!"):
        return "mark", f"MARK {next_mark}", text[1:].strip()
    return "send", text, ""


def open_port(path: str, baud: int) -> int:
    if termios is None:
        raise SystemExit("uart_capture.py працює з портом через termios — лише Linux / macOS "
                         "(на Windows запускати у WSL)")
    fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    attrs = termios.tcgetattr(fd)
    attrs[0] = 0  # iflag: без перетворень і програмного керування потоком
    attrs[1] = 0  # oflag
    attrs[2] = termios.CS8 | termios.CREAD | termios.CLOCAL  # 8N1
    attrs[3] = 0  # lflag: raw
    attrs[4] = attrs[5] = BAUDS[baud]
    attrs[6][termios.VMIN] = 0
    attrs[6][termios.VTIME] = 0
    termios.tcsetattr(fd, termios.TCSANOW, attrs)
    termios.tcflush(fd, termios.TCIFLUSH)
    return fd


def load_summary(cap: Capture, args: argparse.Namespace) -> str:
    if not (args.load_rate > 0 or args.load_burst):
        return "немає"
    mode = "без пауз (--load-burst)" if args.load_burst else f"{args.load_rate:g} Гц"
    if args.load_chunk > 0:
        mode += (f", пакетами по {args.load_chunk} команд з паузою {args.load_chunk_gap:g} с "
                 f"(пауз: {cap.load_chunks})")
    span = (cap.load_last - cap.load_first) if (cap.load_first and cap.load_last) else 0.0
    eff = f", фактично {cap.load_sent / span:.1f} команд/с" if span > 0 else ""
    return (f"профіль {args.load_profile}, {mode}{eff}; надіслано {cap.load_sent}, "
            f"PONG {cap.load_pongs} (втрати PING видно за seq у сирому лозі)")


def render_conditions(cap: Capture, args: argparse.Namespace, t_end: float) -> str:
    start = dt.datetime.fromtimestamp(cap.t0).isoformat(timespec="seconds")
    end = dt.datetime.fromtimestamp(t_end).isoformat(timespec="seconds")
    dur_min = (t_end - cap.t0) / 60.0
    git = cap.git or "не отримано з UART (рядок BUILD) — вписати вручну"
    rows = [
        f"# Умови прогону {args.name}",
        "",
        "| Параметр | Значення |",
        "|---|---|",
        f"| Прогін | {args.name} |",
        f"| Початок запису | {start} |",
        f"| Кінець запису | {end} |",
        f"| Тривалість запису | {dur_min:.1f} хв |",
        f"| git-хеш прошивки | `{git}` |",
        f"| Рядок BUILD | `{cap.build_line or '—'}` |",
        f"| UART | {args.port}, {args.baud} 8N1 |",
        f"| Прийнято | {cap.rx_bytes} Б, {cap.lines} рядків |",
        f"| Сирий лог | `{args.name}_uart_raw.txt` |",
        f"| Навантаження UART | {load_summary(cap, args)} |",
        f"| Лог з часом ПК | `{args.name}_uart_ts.txt` |",
        "",
        "## Заповнити вручну",
        "",
        "- Початковий стан замка і дверей:",
        "- Підключені вузли / що від'єднано:",
        "- Налаштування сенсорів і камери:",
        "- Кількість подій:",
        "- Відхилення від протоколу:",
        "",
        "## MARK (дії оператора)",
        "",
    ]
    if cap.marks:
        rows += ["| t, с | MARK | Дія |", "|---|---|---|"]
        rows += [f"| {t - cap.t0:.1f} | {n} | {d or '—'} |" for t, n, d in cap.marks]
    else:
        rows.append("—")
    rows += ["", "## Надіслані команди", ""]
    if cap.sent:
        rows += ["| t, с | Команда |", "|---|---|"]
        rows += [f"| {t - cap.t0:.1f} | `{c}` |" for t, c in cap.sent]
    else:
        rows.append("—")
    if cap.unanswered:
        rows += ["", "## Команди без відповіді після всіх повторів", ""]
        rows += [f"- {t - cap.t0:.1f} с: `{c}`" for t, c in cap.unanswered]
    if cap.notes:
        rows += ["", "## Нотатки", ""]
        rows += [f"- {t - cap.t0:.1f} с: {n}" for t, n in cap.notes]
    return "\n".join(rows) + "\n"


def run(args: argparse.Namespace) -> int:
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    raw_path = out / f"{args.name}_uart_raw.txt"
    ts_path = out / f"{args.name}_uart_ts.txt"
    cond_path = out / f"{args.name}_conditions.md"
    for p in (raw_path, ts_path, cond_path):
        if p.exists() and not args.force:
            print(f"{p} вже існує — інша назва або --force", file=sys.stderr)
            return 2

    fd = open_port(args.port, args.baud)
    cap = Capture(t0=time.time())
    plan = list(PLANS.get(args.plan, []))
    deadline = cap.t0 + args.duration * 60.0 if args.duration else None
    stdin_ok = args.interactive and sys.stdin.isatty()
    next_mark = 1
    inbox: List[str] = []  # рядки, прийняті після останньої перевіреної команди

    with open(raw_path, "wb") as raw, open(ts_path, "w", encoding="utf-8") as ts:

        def note(text: str) -> None:
            t = time.time()
            ts.write(f"{stamp(t, cap.t0)} # {text}\n")
            ts.flush()
            print(f"[{t - cap.t0:7.1f} с] {text}", flush=True)

        def send(cmd: str, log_cmd: bool = True) -> None:
            data = encode_command(cmd)
            if args.char_gap > 0:
                # USART2 у прошивці опитується без буфера: два байти за одне блокування циклу
                # (~1,6 мс security, рядок printf) дають переповнення. По одному байту — тримається в DR.
                for i in range(len(data)):
                    os.write(fd, data[i:i + 1])
                    time.sleep(args.char_gap)
            else:
                os.write(fd, data)
            if log_cmd:
                cap.sent.append((time.time(), cmd))
            ts.write(f"{stamp(time.time(), cap.t0)} > {cmd}\n")

        last_rx = [time.time()]

        def wait_quiet() -> None:
            """Поки плата друкує (printf блокує цикл), прийом не опитується — чекаємо тиші."""
            t_stop = time.time() + args.quiet_max
            while time.time() < t_stop and time.time() - last_rx[0] < args.quiet:
                pump(0.02)

        def send_checked(cmd: str) -> bool:
            """Надсилає команду; якщо відповідь відома — чекає її і повторює до --retries разів."""
            reply = expected_reply(cmd)
            attempts = args.retries if reply else 1
            for attempt in range(1, attempts + 1):
                wait_quiet()
                inbox.clear()
                send(cmd)
                t_stop = time.time() + args.cmd_wait
                while time.time() < t_stop:
                    pump(0.05)
                    if reply and any(reply.search(line) for line in inbox):
                        return True
                if reply:
                    note(f"немає відповіді на {cmd} (спроба {attempt}/{attempts})")
            if reply:
                cap.unanswered.append((time.time(), cmd))
                return False
            return True

        def pump(timeout: float) -> None:
            rlist = [fd] + ([sys.stdin] if stdin_ok else [])
            ready, _, _ = select.select(rlist, [], [], timeout)
            if fd in ready:
                try:
                    data = os.read(fd, 4096)
                except BlockingIOError:
                    data = b""
                if data:
                    raw.write(data)
                    raw.flush()
                    now = time.time()
                    last_rx[0] = now
                    for line in cap.feed(data):
                        inbox.append(line)
                        ts.write(f"{stamp(now, cap.t0)} {line}\n")
                        if args.echo:
                            print(line, flush=True)
                    ts.flush()
            if stdin_ok and sys.stdin in ready:
                handle_operator(sys.stdin.readline())

        def handle_operator(text: str) -> None:
            nonlocal next_mark
            kind, cmd, desc = parse_operator(text, next_mark)
            if kind == "note":
                cap.notes.append((time.time(), desc))
                note(desc)
            elif kind == "mark":
                cap.marks.append((time.time(), next_mark, desc))
                note(f"MARK {next_mark}: {desc or 'без опису'}")
                send_checked(cmd)
                next_mark += 1
            elif kind == "send":
                send_checked(cmd)

        note(f"старт запису {args.name}, порт {args.port} {args.baud} 8N1")
        if args.start and args.boot_wait > 0:
            # після RESET плата друкує self-test кілька секунд — команди тоді губляться
            t_stop = time.time() + args.boot_wait
            while time.time() < t_stop and cap.git is None:
                pump(0.1)
            t_stop = time.time() + (args.boot_settle if cap.git else 0.0)
            while time.time() < t_stop:
                pump(0.1)
            note("BUILD отримано, надсилаю --start" if cap.git else "BUILD не було — надсилаю --start")
        for cmd in args.start:
            send_checked(cmd)
        load_on = args.load_rate > 0 or args.load_burst
        period = 0.0 if args.load_burst else (1.0 / args.load_rate if load_on else 0.0)
        next_load = time.time()
        step = 1
        in_chunk = 0
        if load_on:
            note(f"навантаження UART: профіль {args.load_profile}, "
                 + ("без пауз" if args.load_burst else f"{args.load_rate:g} Гц")
                 + (f", пакетами по {args.load_chunk} з паузою {args.load_chunk_gap:g} с"
                    if args.load_chunk > 0 else ""))
            if args.load_chunk > 0:
                worst = max(chunk_bytes(s, args.load_chunk, args.load_profile) for s in (1, 251, 9976))
                if worst > STLINK_SAFE_BYTES:
                    note(f"УВАГА: пакет до {worst} Б > {STLINK_SAFE_BYTES} Б — зменште --load-chunk")
        try:
            while deadline is None or time.time() < deadline:
                while plan and time.time() - cap.t0 >= plan[0][0] * 60.0:
                    note(plan.pop(0)[1])
                if load_on and time.time() >= next_load:
                    # без очікування відповіді: це навантаження, а не службова команда
                    send(load_command(step, args.load_profile), log_cmd=False)
                    now = time.time()
                    cap.load_sent += 1
                    cap.load_first = cap.load_first or now
                    cap.load_last = now
                    step += 1
                    next_load = now if args.load_burst else max(next_load + period, now - period)
                    in_chunk, next_load, closed = pace_after_load(
                        in_chunk, args.load_chunk, args.load_chunk_gap, now, next_load)
                    if closed:
                        cap.load_chunks += 1
                    pump(0.0)
                    continue
                pump(min(0.2, max(0.0, next_load - time.time())) if load_on else 0.2)
        except KeyboardInterrupt:
            note("зупинено оператором")
        for cmd in args.end:
            send_checked(cmd)
        for line in cap.flush():
            ts.write(f"{stamp(time.time(), cap.t0)} {line}\n")
        note(f"кінець запису: {cap.lines} рядків, git={cap.git or '?'}")

    os.close(fd)
    cond_path.write_text(render_conditions(cap, args, time.time()), encoding="utf-8")
    print(f"Збережено: {raw_path}, {ts_path}, {cond_path}")
    if cap.git is None:
        print("УВАГА: рядка BUILD не було — git-хеш вписати в conditions вручну", file=sys.stderr)
    if cap.unanswered:
        lost = ", ".join(c for _, c in cap.unanswered)
        print(f"УВАГА: без відповіді після повторів: {lost}", file=sys.stderr)
    return 0


def split_cmds(text: str) -> List[str]:
    return [c.strip() for c in text.split(",") if c.strip()]


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(description="Запис UART-логу стендового прогону SmartLock")
    p.add_argument("--port", required=True, help="напр. /dev/cu.usbmodem12303")
    p.add_argument("--baud", type=int, default=115200, choices=sorted(BAUDS))
    p.add_argument("--name", required=True, help="префікс файлів: S1, S2, S3, E2E-05 ...")
    p.add_argument("--out", default="docs/metrics", help="тека для логів")
    p.add_argument("--duration", type=float, default=0.0, help="хвилин; 0 — до Ctrl+C")
    p.add_argument("--plan", choices=sorted(PLANS), help="нагадування з протоколу")
    p.add_argument("--start", type=split_cmds, default=[], help="команди на старті, через кому")
    p.add_argument("--end", type=split_cmds, default=["STATS", "MEM"],
                   help="команди наприкінці, через кому (типово STATS,MEM; '' — без команд)")
    p.add_argument("--cmd-wait", type=float, default=3.0,
                   help="с очікування відповіді на команду (STATS, MEM, PING, MARK)")
    p.add_argument("--retries", type=int, default=3,
                   help="спроб для команди з відомою відповіддю (прийом UART у прошивці губить команди)")
    p.add_argument("--char-gap", type=float, default=0.005,
                   help="с між символами команди (0 — одним блоком)")
    p.add_argument("--quiet", type=float, default=0.2,
                   help="с тиші на лінії перед відправкою команди")
    p.add_argument("--quiet-max", type=float, default=2.0, help="с найдовшого очікування тиші")
    p.add_argument("--load-rate", type=float, default=0.0,
                   help="Гц навантаження UART для S3 (PING/MARK/MEM без очікування відповіді); 0 — вимкнено")
    p.add_argument("--load-burst", action="store_true",
                   help="навантаження без пауз (методичка §6.2.3); темп обмежує --char-gap")
    p.add_argument("--load-chunk", type=int, default=0,
                   help="подавати навантаження пакетами по N команд з паузою --load-chunk-gap "
                        "(обхід зависання ST-LINK VCP на потоці > ~380 Б; рекомендовано 25 для "
                        "--load-burst і ≥ 50 Гц). 0 — без пакетів, як у прогонах ЛР3. Для порівняння "
                        "ЛР3 / ЛР4 задавати однаково для обох прошивок")
    p.add_argument("--load-chunk-gap", type=float, default=0.3,
                   help="с паузи між пакетами --load-chunk (плата відповідає, буфер ST-LINK спорожнюється)")
    p.add_argument("--load-profile", choices=("ping", "mixed"), default="mixed",
                   help="профіль команд як у scripts/uart_load.py (за замовчуванням mixed)")
    p.add_argument("--boot-wait", type=float, default=10.0,
                   help="с очікування рядка BUILD перед --start (0 — не чекати)")
    p.add_argument("--boot-settle", type=float, default=3.0,
                   help="с після BUILD до --start (self-test плати)")
    p.add_argument("--no-echo", dest="echo", action="store_false", help="не дублювати UART у термінал")
    p.add_argument("--no-input", dest="interactive", action="store_false",
                   help="не читати клавіатуру (запуск у фоні)")
    p.add_argument("--force", action="store_true", help="перезаписати наявні файли")
    return p


def main(argv: Optional[List[str]] = None) -> int:
    return run(build_parser().parse_args(argv))


if __name__ == "__main__":
    sys.exit(main())