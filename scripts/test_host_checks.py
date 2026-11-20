#!/usr/bin/env python3
"""Тести розбору підсумку scripts/host_checks.py (I11) на зразках виводу make і unittest.

Головне — не пропустити падіння: набір з Failures > 0, набір, що завершився аварійно
без рядка Unity, покриття нижче порогу, periph_check / self_test / tools / python FAIL.
"""
import io
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import host_checks as hc  # noqa: E402

UNIT_OK = """\
./build/test_periph_check
  82 checks, 0 failed -> ALL TESTS PASSED
rm -f build/dev/*.gcda
make[1]: Entering directory '/workspace/tests'

======== build/test_bmp180
unit/test_bmp180.c:10:test_a:PASS
-----------------------
14 Tests 0 Failures 0 Ignored
OK

======== build/test_camera
6 Tests 0 Failures 0 Ignored
OK
Self-test formatting: ALL TESTS PASSED

======== coverage: firmware/Devices (gcov, line threshold 90%)
  bmp180.c     lines 100.00% of   86   branches  97.06% of   68
  TOTAL        lines  99.88% of  818 -> OK

======== coverage: firmware/App (gcov, line threshold 90%)
  TOTAL        lines  98.79% of 2570 -> OK
"""

TOOLS_OK = "frame_decode: test pattern matches byte for byte\nframe_decode: OK\n"
PY_OK = "test_x (t.T) ... ok\n\n------\nRan 6 tests in 0.1s\n\nOK\nRan 5 tests in 0.2s\n\nOK\n"


def run(results):
    buf = io.StringIO()
    ok = hc.summarize(results, buf)
    return ok, buf.getvalue()


def all_ok(**over):
    r = {"deps": (0, "check-deps: OK\n", 0.1), "unit": (0, UNIT_OK, 30.0),
         "tools": (0, TOOLS_OK, 2.0), "python": (0, PY_OK, 1.0)}
    r.update(over)
    return r


class HostChecksTest(unittest.TestCase):
    def test_all_pass(self):
        ok, text = run(all_ok())
        self.assertTrue(ok, text)
        self.assertIn("Unity: 2 наборів, 20 тестів, 0 падінь", text)
        self.assertIn("periph_check: 82 перевірок, 0 падінь", text)
        self.assertIn("firmware/App", text)
        self.assertIn("python: 11 тестів, OK", text)
        self.assertIn("I11 HOST CHECKS: PASS", text)

    def test_parsers(self):
        self.assertEqual(hc.parse_unity(UNIT_OK), [("test_bmp180", 14, 0, 0), ("test_camera", 6, 0, 0)])
        self.assertEqual(hc.parse_periph(UNIT_OK), (82, 0))
        self.assertEqual([c[0] for c in hc.parse_coverage(UNIT_OK)], ["firmware/Devices", "firmware/App"])
        self.assertEqual(hc.parse_python(PY_OK), (11, True))
        self.assertIsNone(hc.parse_python("nothing"))

    def test_unity_failures_fail(self):
        unit = UNIT_OK.replace("6 Tests 0 Failures", "6 Tests 2 Failures")
        ok, text = run(all_ok(unit=(2, unit, 30.0)))
        self.assertFalse(ok)
        self.assertIn("FAIL: test_camera: 2 падінь", text)
        self.assertIn("FAIL: крок unit завершився з кодом 2", text)
        self.assertIn("I11 HOST CHECKS: FAIL", text)

    def test_crashed_suite_without_summary_fails(self):
        unit = UNIT_OK.replace("6 Tests 0 Failures 0 Ignored", "Segmentation fault")
        ok, text = run(all_ok(unit=(0, unit, 30.0)))
        self.assertFalse(ok)
        self.assertIn("test_camera: немає рядка", text)

    def test_coverage_below_threshold_fails(self):
        unit = UNIT_OK.replace("98.79% of 2570 -> OK", "88.10% of 2570 -> FAIL")
        ok, text = run(all_ok(unit=(0, unit, 30.0)))
        self.assertFalse(ok)
        self.assertIn("покриття firmware/App нижче порогу", text)

    def test_periph_selftest_tools_python_failures(self):
        for key, bad, msg in (
            ("unit", UNIT_OK.replace("82 checks, 0 failed", "82 checks, 3 failed"), "periph_check: 3 падінь"),
            ("unit", UNIT_OK.replace("Self-test formatting: ALL TESTS PASSED", ""), "self_test не пройшов"),
            ("tools", "frame_decode: FAIL crc32\n", "tools: немає"),
            ("python", "Ran 5 tests in 0.1s\n\nFAILED (failures=1)\n", "python: є падіння"),
            ("python", "SyntaxError\n", "python: немає підсумку"),
        ):
            ok, text = run(all_ok(**{key: (0, bad, 1.0)}))
            self.assertFalse(ok, msg)
            self.assertIn(msg, text)

    def test_empty_unit_output_fails(self):
        ok, text = run(all_ok(unit=(0, "", 0.0)))
        self.assertFalse(ok)
        self.assertIn("не знайдено жодного набору Unity", text)
        self.assertIn("немає рядків TOTAL покриття", text)

    def test_only_selected_steps(self):
        ok, text = run({"deps": (0, "check-deps: OK\n", 0.1)})
        self.assertTrue(ok, text)


if __name__ == "__main__":
    unittest.main(verbosity=2)
