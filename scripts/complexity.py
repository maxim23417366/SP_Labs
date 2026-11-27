#!/usr/bin/env python3
"""
Метрики складності прикладного коду SmartLock для порівняння ЛР3 і ЛР4 (M03, методичка ЛР4 §7.3).

Рахує однаковим способом для будь-якої версії коду — прямо з git, без checkout:

  1. Рядки коду в firmware/App (*.c, *.h): фізичні, порожні, коментарі, код
     (як cloc: рядок, де є і код, і коментар, рахується кодом).
  2. Глобальні змінні, які змінюються в контексті переривань: змінні рівня файлу
     (static чи глобальні) з firmware/{App,Platform,Devices,Core/Src}, яким присвоюють
     значення (=, op=, ++, --) у функціях контексту ISR. Контекст ISR — обробники
     *_IRQHandler / *_Handler, колбеки HAL_*Callback, хуки FreeRTOS, що викликаються з
     переривання (vApplicationTickHook), і функції, які вони викликають (до 3 рівнів)
     у тих самих каталогах. Запис через вказівник скрипт не бачить — це обмеження методу.

«Скільки місць правити, щоб додати періодичну задачу» автоматично не рахується —
див. docs/metrics/complexity.md.

Приклад:
  python3 scripts/complexity.py --ref lab3 --ref main --md docs/metrics/complexity_auto.md
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Dict, List, Optional, Set, Tuple

LOC_DIR = "firmware/App"
ISR_DIRS = ("firmware/App", "firmware/Platform", "firmware/Devices", "firmware/Core/Src")
ISR_ROOT_RE = re.compile(r"^(\w+_IRQHandler|\w+_Handler|HAL_\w*Callback|vApplicationTickHook)$")
NOT_ISR = {"Reset_Handler", "Error_Handler"}   # Error_Handler викликає код HAL, а не NVIC
CALL_DEPTH = 3
C_KEYWORDS = {
    "if", "for", "while", "switch", "return", "sizeof", "do", "else", "case", "default",
    "break", "continue", "goto", "static", "const", "volatile", "extern", "struct", "union",
    "enum", "typedef", "unsigned", "signed", "inline",
}


# ------------------------------------------------------------------ джерела

class Source:
    """Файли однієї версії коду: git-ref або каталог."""

    def __init__(self, ref: Optional[str] = None, root: Optional[Path] = None, repo: Path = Path(".")):
        self.ref, self.root, self.repo = ref, root, repo
        self.label = ref if ref else str(root)

    def list(self, directory: str) -> List[str]:
        if self.ref:
            out = subprocess.run(
                ["git", "-C", str(self.repo), "ls-tree", "-r", "--name-only", self.ref, directory],
                check=True, capture_output=True, text=True).stdout
            return sorted(p for p in out.splitlines() if p)
        base = self.root / directory
        return sorted(str(p.relative_to(self.root)) for p in base.rglob("*") if p.is_file()) if base.exists() else []

    def read(self, path: str) -> str:
        if self.ref:
            return subprocess.run(["git", "-C", str(self.repo), "show", f"{self.ref}:{path}"],
                                  check=True, capture_output=True).stdout.decode("utf-8", "replace")
        return (self.root / path).read_text(encoding="utf-8", errors="replace")

    def describe(self) -> str:
        if not self.ref:
            return self.label
        try:
            h = subprocess.run(["git", "-C", str(self.repo), "rev-parse", "--short", self.ref],
                               check=True, capture_output=True, text=True).stdout.strip()
            return f"{self.ref} ({h})" if h != self.ref else h
        except subprocess.CalledProcessError:
            return self.ref


# --------------------------------------------------------------- коментарі C

def split_code_comments(text: str) -> List[Tuple[str, bool]]:
    """Для кожного рядка: (код без коментарів, чи був у рядку коментар). Рядки й символи C враховано."""
    out: List[Tuple[str, bool]] = []
    in_block = False
    for line in text.splitlines():
        code: List[str] = []
        had_comment = in_block
        i, n = 0, len(line)
        quote: Optional[str] = None
        while i < n:
            c = line[i]
            if in_block:
                if line.startswith("*/", i):
                    in_block = False
                    i += 2
                else:
                    i += 1
                continue
            if quote:
                code.append(c)
                if c == "\\" and i + 1 < n:
                    code.append(line[i + 1])
                    i += 2
                    continue
                if c == quote:
                    quote = None
                i += 1
                continue
            if line.startswith("//", i):
                had_comment = True
                break
            if line.startswith("/*", i):
                in_block = True
                had_comment = True
                i += 2
                continue
            if c in "\"'":
                quote = c
            code.append(c)
            i += 1
        out.append(("".join(code), had_comment))
    return out


@dataclass
class Loc:
    files: int = 0
    physical: int = 0
    blank: int = 0
    comment: int = 0
    code: int = 0


def count_loc(src: Source, directory: str = LOC_DIR) -> Loc:
    loc = Loc()
    for path in src.list(directory):
        if not path.endswith((".c", ".h")):
            continue
        loc.files += 1
        for code, had_comment in split_code_comments(src.read(path)):
            loc.physical += 1
            if code.strip():
                loc.code += 1
            elif had_comment:
                loc.comment += 1
            else:
                loc.blank += 1
    return loc


# ------------------------------------------------------- глобальні й функції

RE_FUNC_HEAD = re.compile(r"(\w+)\s*\([^;{}]*\)\s*$", re.S)
RE_CALL = re.compile(r"\b([A-Za-z_]\w*)\s*\(")


@dataclass
class CFile:
    path: str
    globals: Set[str] = field(default_factory=set)
    functions: Dict[str, str] = field(default_factory=dict)   # ім'я -> тіло


def parse_c(path: str, text: str) -> CFile:
    """Груба, але детермінована розбивка: верхній рівень файлу -> оголошення змінних і тіла функцій."""
    lines = split_code_comments(text)
    code = "\n".join(l for l, _ in lines)
    code = re.sub(r"^\s*#.*$", "", code, flags=re.M)          # препроцесор
    cf = CFile(path)
    depth = 0
    buf: List[str] = []       # текст верхнього рівня до '{' або ';'
    body: List[str] = []
    fname: Optional[str] = None
    in_initializer = False
    for ch in code:
        if depth == 0:
            if ch == "{":
                head = "".join(buf)
                m = RE_FUNC_HEAD.search(head)
                if m and "=" not in head and m.group(1) not in C_KEYWORDS:
                    fname = m.group(1)
                    body = []
                    in_initializer = False
                else:
                    fname = None
                    in_initializer = "=" in head      # ініціалізатор масиву / структури
                    if not in_initializer:
                        buf = []                       # struct / enum / union: тіло пропускаємо
                depth = 1
                continue
            if ch == ";":
                add_declaration(cf, "".join(buf))
                buf = []
                continue
            buf.append(ch)
        else:
            if ch == "{":
                depth += 1
            elif ch == "}":
                depth -= 1
                if depth == 0:
                    if fname:
                        cf.functions[fname] = "".join(body)
                        buf = []
                    elif not in_initializer:
                        buf.append(" ")                # «struct {…} name;» — ім'я після '}'
                    fname = None
                    continue
            if fname:
                body.append(ch)
    return cf


def add_declaration(cf: CFile, decl: str) -> None:
    d = " ".join(decl.split())
    if not d or d.startswith(("typedef", "extern")) or "(" in d.split("=")[0]:
        return    # тип, зовнішнє оголошення, прототип чи вказівник на функцію
    left = d.split("=", 1)[0]
    for part in left.split(","):
        m = re.search(r"([A-Za-z_]\w*)\s*(\[[^\]]*\]\s*)*$", part.strip())
        if m and m.group(1) not in C_KEYWORDS:
            cf.globals.add(m.group(1))


def writes_to(body: str, name: str) -> bool:
    n = re.escape(name)
    pat = (rf"(\+\+|--)\s*{n}\b|\b{n}\s*(\[[^\]]*\]\s*)*(\.\w+\s*)*"
           rf"(\+\+|--|(\+|-|\*|/|%|&|\||\^|<<|>>)?=(?!=))")
    return re.search(pat, body) is not None


@dataclass
class IsrReport:
    roots: List[str]
    context: List[str]
    written: List[Tuple[str, str, List[str]]]   # (змінна, файл, функції ISR, що пишуть)


def isr_globals(src: Source) -> IsrReport:
    files: List[CFile] = []
    for d in ISR_DIRS:
        for path in src.list(d):
            if path.endswith(".c"):
                files.append(parse_c(path, src.read(path)))
    funcs: Dict[str, Tuple[CFile, str]] = {}
    for cf in files:
        for fn, body in cf.functions.items():
            funcs.setdefault(fn, (cf, body))

    roots = sorted(f for f in funcs if ISR_ROOT_RE.match(f) and f not in NOT_ISR)
    context: Set[str] = set(roots)
    frontier = set(roots)
    for _ in range(CALL_DEPTH):
        nxt = set()
        for f in frontier:
            for callee in RE_CALL.findall(funcs[f][1]):
                if callee in funcs and callee not in context and callee not in C_KEYWORDS:
                    nxt.add(callee)
        context |= nxt
        frontier = nxt

    written: List[Tuple[str, str, List[str]]] = []
    for cf in files:
        for g in sorted(cf.globals):
            writers = sorted(f for f in context if funcs[f][0] is cf and writes_to(funcs[f][1], g))
            # глобальна змінна може писатись і в ISR іншого файлу (не static)
            writers += sorted(f for f in context if funcs[f][0] is not cf
                              and g not in funcs[f][0].globals and writes_to(funcs[f][1], g)
                              and f not in writers)
            if writers:
                written.append((g, cf.path, writers))
    return IsrReport(roots, sorted(context), written)


# ------------------------------------------------------------------- звіт

def render(results: List[Tuple[Source, Loc, IsrReport]]) -> str:
    out = ["# Метрики складності (автоматично, `scripts/complexity.py`)", "",
           "| Метрика | " + " | ".join(f"`{s.describe()}`" for s, _, _ in results) + " |",
           "| :--- | " + " | ".join("---:" for _ in results) + " |"]
    rows = [
        (f"файлів `*.c` / `*.h` у `{LOC_DIR}`", lambda l, i: l.files),
        ("рядків коду (без порожніх і коментарів)", lambda l, i: l.code),
        ("рядків коментарів", lambda l, i: l.comment),
        ("фізичних рядків", lambda l, i: l.physical),
        ("функцій у контексті переривань", lambda l, i: len(i.context)),
        ("глобальних змінних, що змінюються в перериваннях", lambda l, i: len(i.written)),
    ]
    for title, fn in rows:
        out.append(f"| {title} | " + " | ".join(str(fn(l, i)) for _, l, i in results) + " |")
    for s, _, i in results:
        out += ["", f"## `{s.describe()}`: змінні, які змінюються в перериваннях", "",
                "| Змінна | Файл | Хто змінює |", "| :--- | :--- | :--- |"]
        for g, path, writers in i.written:
            out.append(f"| `{g}` | `{path}` | {', '.join(f'`{w}`' for w in writers)} |")
        out += ["", "Корені контексту переривань: " + ", ".join(f"`{r}`" for r in i.roots) + "."]
    return "\n".join(out) + "\n"


def main(argv: Optional[List[str]] = None) -> int:
    ap = argparse.ArgumentParser(description="Метрики складності SmartLock (ЛР4 §7.3)")
    ap.add_argument("--ref", action="append", default=[], help="git-ref (тег, гілка, коміт); можна кілька")
    ap.add_argument("--dir", action="append", type=Path, default=[], help="корінь робочої копії замість git-ref")
    ap.add_argument("--repo", type=Path, default=Path("."), help="репозиторій для --ref (типово поточний)")
    ap.add_argument("--md", type=Path, help="записати Markdown у файл")
    args = ap.parse_args(argv)
    sources = [Source(ref=r, repo=args.repo) for r in args.ref] + [Source(root=d) for d in args.dir]
    if not sources:
        sources = [Source(ref="lab3", repo=args.repo)]
    try:
        results = [(s, count_loc(s), isr_globals(s)) for s in sources]
    except subprocess.CalledProcessError as e:
        print(f"Помилка git: {e.stderr.strip() if e.stderr else e}", file=sys.stderr)
        return 2
    text = render(results)
    if args.md:
        args.md.write_text(text, encoding="utf-8")
        print(f"complexity: записано {args.md}")
    else:
        sys.stdout.write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())