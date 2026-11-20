#!/usr/bin/env python3
"""ЛР3 (I11): усі перевірки без плати одним запуском і один лог для звіту.

Запуск — з кореня репозиторію в Docker-образі stm32-builder (gcc, gcov, python3):

    make host-checks                 # Docker, лог: tests/build/I11_host_checks.log
    python3 scripts/host_checks.py --git <хеш> --out tests/build/I11_host_checks.log

Кроки (той самий порядок у CI, .github/workflows/ci.yml):

    1. deps      scripts/check_deps.sh — правила залежностей шарів
    2. unit      make -C tests periph selftest coverage — periph_check, усі Unity-набори
                 (Devices, Platform, App), self_test і покриття gcov з порогами
    3. tools     make -C tests tools — кадр C → UART-лог → frame_decode.py → PNG
    4. python    unittest для scripts/test_*.py і tools/test_*.py

Лог містить повний вивід кожного кроку, а в кінці — підсумок: набори Unity
(тестів / падінь), periph_check, покриття за областями, Python-тести, час кроків,
загальний результат. Код виходу 0 — усе пройшло; 1 — хоч одна перевірка впала
(або підсумок не вдалося розібрати, наприклад, набір упав, не дійшовши до
підсумкового рядка Unity).
"""
import argparse
import datetime
import os
import re
import subprocess
import sys
import time

RE_SUITE = re.compile(r"^======== (?:\./)?(?:build/)?(test_\w+)\s*$")
RE_UNITY = re.compile(r"^(\d+) Tests (\d+) Failures (\d+) Ignored\s*$")
RE_PERIPH = re.compile(r"(\d+) checks, (\d+) failed")
RE_COV_HDR = re.compile(r"^======== coverage: (.+?) \(gcov")
RE_COV_TOTAL = re.compile(r"^\s*TOTAL\s+lines\s+([\d.]+)% of\s+(\d+)\s+->\s+(\w+)")
RE_PY_RAN = re.compile(r"^Ran (\d+) tests? in")
RE_PY_RESULT = re.compile(r"^(OK|FAILED)\b(.*)$")


def parse_unity(text):
    """Набори Unity: [(ім'я, тестів, падінь, ignored)]; ім'я з рядка «======== build/test_x»."""
    suites = []
    current = None
    for line in text.splitlines():
        m = RE_SUITE.match(line.strip())
        if m:
            current = m.group(1)
            continue
        m = RE_UNITY.match(line.strip())
        if m and current is not None:
            suites.append((current, int(m.group(1)), int(m.group(2)), int(m.group(3))))
            current = None
    return suites


def started_suites(text):
    """Імена всіх наборів, що почали виконуватися (щоб знайти ті, що впали без підсумку)."""
    return [m.group(1) for m in (RE_SUITE.match(l.strip()) for l in text.splitlines()) if m]


def parse_periph(text):
    m = RE_PERIPH.search(text)
    return (int(m.group(1)), int(m.group(2))) if m else None


def parse_coverage(text):
    """[(область, відсоток рядків, рядків, OK|FAIL)] у порядку виводу."""
    out = []
    area = None
    for line in text.splitlines():
        m = RE_COV_HDR.match(line.strip())
        if m:
            area = m.group(1)
            continue
        m = RE_COV_TOTAL.match(line)
        if m and area is not None:
            out.append((area, float(m.group(1)), int(m.group(2)), m.group(3)))
            area = None
    return out


def parse_python(text):
    """(тестів, OK?) з виводу unittest; None — підсумку немає."""
    ran = None
    ok = None
    for line in text.splitlines():
        m = RE_PY_RAN.match(line.strip())
        if m:
            ran = (ran or 0) + int(m.group(1))
        m = RE_PY_RESULT.match(line.strip())
        if m:
            ok = (m.group(1) == "OK") if ok is None else (ok and m.group(1) == "OK")
    return None if ran is None else (ran, bool(ok))


def summarize(results, out):
    """results: {крок: (код виходу, вивід, секунд)}. Пише підсумок у out, повертає True/False."""
    problems = []
    w = out.write

    w("\n==================== I11: ПІДСУМОК HOST-ПЕРЕВІРОК ====================\n")
    for step, (rc, _, sec) in results.items():
        w("  %-7s exit=%d  %6.1f s\n" % (step, rc, sec))
        if rc != 0:
            problems.append("крок %s завершився з кодом %d" % (step, rc))

    unit_text = results.get("unit", (0, "", 0.0))[1]
    suites = parse_unity(unit_text)
    lost = [s for s in started_suites(unit_text) if s not in {x[0] for x in suites}]
    tests = sum(s[1] for s in suites)
    fails = sum(s[2] for s in suites)
    w("\n  Unity: %d наборів, %d тестів, %d падінь\n" % (len(suites), tests, fails))
    for name, n, f, ign in suites:
        w("    %-24s %4d тестів  %d падінь%s\n" % (name, n, f, ("  %d ignored" % ign) if ign else ""))
        if f:
            problems.append("%s: %d падінь" % (name, f))
    for name in lost:
        w("    %-24s без підсумку Unity (аварійне завершення?)\n" % name)
        problems.append("%s: немає рядка «N Tests N Failures»" % name)
    if "unit" in results and not suites:
        problems.append("не знайдено жодного набору Unity")

    periph = parse_periph(unit_text)
    if periph is None:
        if "unit" in results:
            problems.append("periph_check: немає підсумку")
    else:
        w("  periph_check: %d перевірок, %d падінь\n" % periph)
        if periph[1]:
            problems.append("periph_check: %d падінь" % periph[1])
    if "unit" in results:
        st_ok = "Self-test formatting: ALL TESTS PASSED" in unit_text
        w("  self_test: %s\n" % ("PASS" if st_ok else "FAIL"))
        if not st_ok:
            problems.append("self_test не пройшов")

    cov = parse_coverage(unit_text)
    if cov:
        w("\n  Покриття (рядки):\n")
    for area, pct, lines, verdict in cov:
        w("    %-58s %6.2f %% з %5d  %s\n" % (area, pct, lines, verdict))
        if verdict != "OK":
            problems.append("покриття %s нижче порогу" % area)
    if "unit" in results and not cov:
        problems.append("немає рядків TOTAL покриття")

    if "tools" in results:
        t_text = results["tools"][1]
        t_ok = "frame_decode: OK" in t_text
        w("\n  tools: frame_decode %s\n" % ("OK" if t_ok else "FAIL"))
        if not t_ok:
            problems.append("tools: немає «frame_decode: OK»")

    if "python" in results:
        py = parse_python(results["python"][1])
        if py is None:
            problems.append("python: немає підсумку unittest")
        else:
            w("  python: %d тестів, %s\n" % (py[0], "OK" if py[1] else "FAILED"))
            if not py[1]:
                problems.append("python: є падіння")

    w("\n")
    for p in problems:
        w("  FAIL: %s\n" % p)
    verdict = not problems
    w("I11 HOST CHECKS: %s — %d наборів Unity / %d тестів, periph_check, self_test, покриття, tools, python\n"
      % ("PASS" if verdict else "FAIL", len(suites), tests))
    return verdict


def run_step(cmd, out):
    """Виконати команду, дублюючи вивід у консоль і лог; повертає (код, вивід, секунди)."""
    t0 = time.monotonic()
    out.write("\n$ %s\n" % cmd)
    out.flush()
    p = subprocess.Popen(cmd, shell=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                         universal_newlines=True, errors="replace")
    chunks = []
    for line in p.stdout:
        chunks.append(line)
        out.write(line)
        sys.stdout.write(line)
    p.wait()
    out.flush()
    return p.returncode, "".join(chunks), time.monotonic() - t0


class Tee:
    def __init__(self, f):
        self.f = f

    def write(self, s):
        self.f.write(s)

    def flush(self):
        self.f.flush()


STEPS = [
    ("deps", "tr -d '\\r' < scripts/check_deps.sh | sh"),
    ("unit", "make -C tests periph selftest coverage"),
    ("tools", "make -C tests tools"),
    # обидва каталоги — навіть якщо перший упав; код виходу — АБО обох
    ("python", "python3 -m unittest discover -s scripts -p 'test_*.py' -v; r1=$?; "
               "python3 -m unittest discover -s tools -p 'test_*.py' -v; r2=$?; "
               "exit $((r1 | r2))"),
]


def tool_versions():
    cmds = ["gcc --version | head -1", "gcov --version | head -1", "python3 --version",
            "arm-none-eabi-gcc --version 2>/dev/null | head -1 || true"]
    out = []
    for c in cmds:
        try:
            v = subprocess.run(c, shell=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                               universal_newlines=True).stdout.strip()
        except OSError:
            v = ""
        if v:
            out.append(v)
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--git", default=os.environ.get("GIT_HASH", "nogit"), help="git-хеш збірки")
    ap.add_argument("--out", default="tests/build/I11_host_checks.log", help="файл логу")
    ap.add_argument("--only", nargs="*", choices=[s for s, _ in STEPS], help="лише ці кроки")
    args = ap.parse_args(argv)

    os.makedirs(os.path.dirname(args.out) or ".", exist_ok=True)
    with open(args.out, "w", encoding="utf-8", newline="\n") as f:
        out = Tee(f)
        now = datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
        out.write("I11 HOST CHECKS git=%s utc=%s\n" % (args.git, now))
        for v in tool_versions():
            out.write("  tool: %s\n" % v)
        results = {}
        for name, cmd in STEPS:
            if args.only and name not in args.only:
                continue
            out.write("\n==================== I11 крок: %s ====================" % name)
            results[name] = run_step(cmd, out)
        summary = []

        class Both:
            def write(self, s):
                f.write(s)
                summary.append(s)

        ok = summarize(results, Both())
    sys.stdout.write("".join(summary))
    sys.stdout.write("лог: %s\n" % args.out)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
