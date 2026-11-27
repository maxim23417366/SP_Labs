"""H06: правила scripts/check_firmware.sh для ЛР4 (FreeRTOS) без тулчейна ARM.

Справжній arm-none-eabi-nm підміняється скриптом, що друкує заданий список символів,
firmware.elf — файлом із рядком банера. Так перевіряються всі гілки правила «FreeRTOS
присутній» і перехідний режим RTOS_REQUIRED=0 (до K02), а також що решта перевірок
(сильні модулі, git-хеш) працює як раніше.
"""

import os
import shutil
import stat
import subprocess
import tempfile
import unittest

SCRIPT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "check_firmware.sh")
HASH = "abc1234"

MODULES = [
    "SmartLock_FsmStep", "SmartLock_ButtonsPoll", "SmartLock_SecurityPoll",
    "SmartLock_EnvPoll", "SmartLock_StoragePoll", "SmartLock_CameraPoll",
    "SmartLock_CameraRequest", "SmartLock_PrintDiag", "Camera_PrintDiag",
]
SUPERLOOP = ["main", "App_Init", "App_Loop", "Sched_RunOnce", "AppTasks_Init"]
RTOS_START = ["main", "App_Init", "xTaskCreateStatic", "vTaskStartScheduler"]


class CheckFirmwareTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="fwcheck_")
        self.fw = os.path.join(self.tmp, "firmware")
        os.makedirs(os.path.join(self.fw, "build"))
        self.nm = os.path.join(self.tmp, "fake-nm")
        self.syms = os.path.join(self.tmp, "syms.txt")
        with open(self.nm, "w") as f:
            f.write('#!/bin/sh\ncat "%s"\n' % self.syms)
        os.chmod(self.nm, os.stat(self.nm).st_mode | stat.S_IEXEC)

    def tearDown(self):
        shutil.rmtree(self.tmp)

    def build(self, symbols, kernel_dir=False, lab="LR4", weak=()):
        """«Збірка»: артефакти, мітка хеша, банер у .elf і таблиця символів."""
        b = os.path.join(self.fw, "build")
        for name in ("firmware.hex", "firmware.bin"):
            with open(os.path.join(b, name), "wb") as f:
                f.write(b"\x00\x01")
        with open(os.path.join(b, "firmware.elf"), "wb") as f:
            f.write(b"\x7fELF\x00BUILD,fw=SmartLock,lab=%s,git=%s,cpu_hz=%%lu\x00"
                    % (lab.encode(), HASH.encode()))
        open(os.path.join(b, ".git_hash_" + HASH), "w").close()
        with open(self.syms, "w") as f:
            for i, s in enumerate(symbols):
                f.write("%08x %s %s\n" % (0x08000000 + 4 * i, "W" if s in weak else "T", s))
        if kernel_dir:
            os.makedirs(os.path.join(self.fw, "Middlewares", "FreeRTOS"))

    def run_check(self, **env_extra):
        env = dict(os.environ, FW=self.fw, NM=self.nm, GIT_HASH=HASH, REQUIRE_GIT="1")
        env.update(env_extra)
        p = subprocess.run(["sh", SCRIPT], env=env, capture_output=True, text=True)
        return p.returncode, p.stdout

    # --- ЛР4: ядро є ---------------------------------------------------------
    def test_freertos_present_passes(self):
        self.build(RTOS_START + MODULES, kernel_dir=True)
        rc, out = self.run_check(RTOS_REQUIRED="1")
        self.assertEqual(rc, 0, out)
        self.assertIn("OK   — FreeRTOS: xTaskCreateStatic, vTaskStartScheduler", out)
        self.assertIn("старт: main -> App_Init -> vTaskStartScheduler", out)
        self.assertIn("PASS — єдина прошивка STM32F411, FreeRTOS, git=" + HASH, out)

    def test_dynamic_xtaskcreate_also_accepted(self):
        syms = ["main", "App_Init", "xTaskCreate", "vTaskStartScheduler"] + MODULES
        self.build(syms, kernel_dir=True)
        rc, out = self.run_check(RTOS_REQUIRED="1")
        self.assertEqual(rc, 0, out)

    def test_lr3_banner_still_accepted(self):
        self.build(RTOS_START + MODULES, kernel_dir=True, lab="LR3")
        rc, out = self.run_check(RTOS_REQUIRED="1")
        self.assertEqual(rc, 0, out)

    # --- ЛР4: ядра немає ------------------------------------------------------
    def test_no_rtos_fails_when_required(self):
        self.build(SUPERLOOP + MODULES)
        rc, out = self.run_check(RTOS_REQUIRED="1")
        self.assertEqual(rc, 1, out)
        self.assertIn("FAIL — FreeRTOS немає", out)

    def test_default_is_required(self):
        self.build(SUPERLOOP + MODULES)
        rc, out = self.run_check()
        self.assertEqual(rc, 1, out)

    def test_no_rtos_transition_warns(self):
        self.build(SUPERLOOP + MODULES)
        rc, out = self.run_check(RTOS_REQUIRED="0")
        self.assertEqual(rc, 0, out)
        self.assertIn("WARN — FreeRTOS ще немає", out)
        self.assertIn("Super Loop: main -> App_Init", out)
        self.assertIn("PASS", out)

    # --- часткове ядро — завжди помилка ---------------------------------------
    def test_symbols_without_kernel_dir_fail(self):
        self.build(RTOS_START + MODULES, kernel_dir=False)
        for req in ("0", "1"):
            rc, out = self.run_check(RTOS_REQUIRED=req)
            self.assertEqual(rc, 1, out)
            self.assertIn("частково", out)
            self.assertIn("Middlewares/FreeRTOS/", out)

    def test_kernel_dir_without_scheduler_fails(self):
        self.build(SUPERLOOP + MODULES + ["xTaskCreate"], kernel_dir=True)
        rc, out = self.run_check(RTOS_REQUIRED="0")
        self.assertEqual(rc, 1, out)
        self.assertIn("бракує: vTaskStartScheduler", out)

    def test_other_rtos_fails(self):
        self.build(RTOS_START + MODULES + ["tx_kernel_enter"], kernel_dir=True)
        rc, out = self.run_check(RTOS_REQUIRED="1")
        self.assertEqual(rc, 1, out)
        self.assertIn("інша RTOS", out)

    # --- решта перевірок без змін ---------------------------------------------
    def test_weak_module_fails(self):
        self.build(RTOS_START + MODULES, kernel_dir=True, weak={"SmartLock_CameraPoll"})
        rc, out = self.run_check(RTOS_REQUIRED="1")
        self.assertEqual(rc, 1, out)
        self.assertIn("SmartLock_CameraPoll(W)", out)

    def test_wrong_hash_fails(self):
        self.build(RTOS_START + MODULES, kernel_dir=True)
        rc, out = self.run_check(RTOS_REQUIRED="1", GIT_HASH="abc12")
        self.assertEqual(rc, 1, out)

    def test_dirty_banner_not_accepted_for_clean_hash(self):
        self.build(RTOS_START + MODULES, kernel_dir=True)
        elf = os.path.join(self.fw, "build", "firmware.elf")
        with open(elf, "wb") as f:
            f.write(b"\x00BUILD,fw=SmartLock,lab=LR4,git=%s-dirty,\x00" % HASH.encode())
        rc, out = self.run_check(RTOS_REQUIRED="1")
        self.assertEqual(rc, 1, out)


if __name__ == "__main__":
    unittest.main()
