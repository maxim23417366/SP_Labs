#!/usr/bin/env python3
"""Автотести для scripts/complexity.py (M03, ЛР4 §7.3) на штучному коді з відомою відповіддю."""

import tempfile
import unittest
from pathlib import Path

from complexity import Source, count_loc, isr_globals, split_code_comments

APP_C = """\
/* заголовок
 * у два рядки */
#include "app.h"

static int s_count;            // лічильник
int g_mode = 1;

int App_Step(void)
{
    const char *s = "не // коментар";
    s_count++;   /* код і коментар в одному рядку */

    return s_count;
}
"""

APP_H = """\
#pragma once
int App_Step(void);
"""

PLATFORM_C = """\
#include "platform.h"

typedef struct { int a; } pair_t;
static volatile unsigned ev_pending[3];
static volatile unsigned ev_lost;
static unsigned only_main;
static pair_t s_pair;
static const int k_table[2] = { 1, 2 };
void (*s_hook)(void);
int Platform_Read(int x);

static void ev_mark(int i)
{
    if (ev_pending[i]) {
        ev_lost++;
    }
    ev_pending[i] = 1U;
    s_pair.a = i;
}

void EXTI0_IRQHandler(void)
{
    ev_mark(0);
}

void Reset_Handler(void)
{
    only_main = 5;
}

int Platform_Read(int x)
{
    only_main += x;
    return ev_pending[x] == 1U;
}
"""


class ComplexityTest(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        root = Path(self._tmp.name)
        (root / "firmware/App").mkdir(parents=True)
        (root / "firmware/Platform").mkdir(parents=True)
        (root / "firmware/App/app.c").write_text(APP_C, encoding="utf-8")
        (root / "firmware/App/app.h").write_text(APP_H, encoding="utf-8")
        (root / "firmware/Platform/platform_io.c").write_text(PLATFORM_C, encoding="utf-8")
        self.src = Source(root=root)

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def test_comment_stripping_respects_strings(self) -> None:
        lines = split_code_comments('x = "a // b"; // c\n/* d */ y = 1;')
        self.assertEqual(lines[0], ('x = "a // b"; ', True))
        self.assertEqual(lines[1][0].strip(), "y = 1;")

    def test_loc(self) -> None:
        loc = count_loc(self.src)
        self.assertEqual(loc.files, 2)
        self.assertEqual(loc.physical, 14 + 2)
        self.assertEqual(loc.comment, 2)        # два рядки блочного коментаря
        self.assertEqual(loc.blank, 3)
        self.assertEqual(loc.code, 16 - 2 - 3)  # «s_count++; /* … */» — код

    def test_isr_globals(self) -> None:
        rep = isr_globals(self.src)
        self.assertEqual(rep.roots, ["EXTI0_IRQHandler"])        # Reset_Handler — не переривання
        self.assertIn("ev_mark", rep.context)
        written = {g: w for g, _, w in rep.written}
        self.assertEqual(set(written), {"ev_pending", "ev_lost", "s_pair"})
        self.assertEqual(written["ev_lost"], ["ev_mark"])
        self.assertNotIn("only_main", written)                    # пише лише код поза ISR
        self.assertNotIn("k_table", written)


if __name__ == "__main__":
    unittest.main()