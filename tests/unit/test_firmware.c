/**
  * @file    test_firmware.c
  * @brief   ЛР3 (I10): єдина прошивка SmartLock на ПК — той самий набір файлів App/, що й
  *          у firmware.mk (tests/Makefile, APP_OBJS_firmware = усі *.c з App/).
  *
  *          Без RTOS: main.c → App_Init() → while (1) App_Loop() — тут так само,
  *          App_Loop() у циклі з модельним часом. Залізо — fake_platform, fake_i2c
  *          (LSM303D, OV7670, L3GD20; MPU-6050 і BMP180 відсутні, як у журналі ЛР2),
  *          fake_w25q64 (модель W25Q64 у RAM) і fake_camera (DVP).
  *
  *          Перевіряє:
  *            - старт: перший рядок BUILD з git-хешем, 8 задач Super Loop, сильні
  *              реалізації всіх точок підключення (немає слабких заглушок);
  *            - команду VER і рядки DIAG у STATS;
  *            - повний сценарій: LOCK → LOCKED → PIR → тривога → кадр у W25Q64 з ID
  *              події → CAM LOAD → STATS без втрат;
  *            - годину роботи циклу: час монотонний, черги без втрат.
  */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "unity.h"
#include "app.h"
#include "app_config.h"
#include "app_tasks.h"
#include "camera.h"
#include "capture_vectors.h"
#include "cmd.h"
#include "event_queue.h"
#include "evt.h"
#include "fake_camera.h"
#include "fake_delay.h"
#include "fake_i2c.h"
#include "fake_platform.h"
#include "fake_w25q64.h"
#include "frame_export.h"
#include "frame_store.h"
#include "mock_spi.h"
#include "ov7670.h"
#include "perf.h"
#include "sched.h"
#include "self_test.h"
#include "sl_buttons.h"
#include "sl_camera_link.h"
#include "sl_env.h"
#include "sl_fsm.h"
#include "sl_journal.h"
#include "sl_security.h"
#include "smartlock_hooks.h"

/* ---- перехоплення stdout ------------------------------------------------------- */
static char out[1 << 17];
static const char *line_cmd;

static const char *capture(void (*fn)(void))
{
  FILE *tmp = tmpfile();
  int saved;
  size_t n;

  out[0] = '\0';
  if (tmp == NULL) { return out; }
  fflush(stdout);
  saved = dup(1);
  dup2(fileno(tmp), 1);
  fn();
  fflush(stdout);
  dup2(saved, 1);
  close(saved);
  rewind(tmp);
  n = fread(out, 1U, sizeof(out) - 1U, tmp);
  out[n] = '\0';
  fclose(tmp);
  return out;
}

static void run_line(void) { Cmd_Execute(line_cmd); }

static const char *cmd(const char *line)
{
  line_cmd = line;
  return capture(run_line);
}

/* Super Loop як у main.c: App_Loop() раз на 10 мс модельного часу */
static uint32_t s_t;
static uint32_t s_run_ms;

static void run_loop(void)
{
  for (uint32_t end = s_t + s_run_ms; s_t < end; s_t += 10U)
  {
    fake_platform.now_ms = s_t;
    App_Loop();
  }
}

static const char *loop_ms(uint32_t ms)
{
  s_run_ms = ms;
  return capture(run_loop);
}

static void boot(void)
{
  App_Init();
}

/* ---- стенд: модулі ЛР2 + камера, Flash — модель W25Q64 --------------------------- */
static void bench(void)
{
  uint8_t *r = fake_i2c_add_device(LSM303D_I2C_ADDR);
  uint8_t *cam;

  r[LSM303D_REG_WHO_AM_I] = LSM303D_WHO_AM_I_VALUE;
  memcpy(&r[LSM303D_REG_OUT_X_L_A | LSM303D_AUTO_INC], &cap_st_lsm303d_xyz_wire[CAP_ST_LSM303D_XYZ_OFFSET], 6U);
  fake_i2c_add_device(0x6BU)[0x0FU] = 0xD4U;
  cam = fake_i2c_add_device(OV7670_I2C_ADDR);
  cam[OV7670_REG_PID] = OV7670_PID_VALUE;
  cam[OV7670_REG_VER] = OV7670_VER_VALUE;
  fake_platform.vsync_hz    = 30U;
  fake_platform.flash_model = 1;
}

void setUp(void)
{
  fake_platform_reset();
  fake_i2c_reset();
  mock_spi_reset();
  fake_w25q64_reset();
  fake_delay_reset();
  fake_camera_reset();
  memset(&app_devices, 0, sizeof(app_devices));
  app_devices.mpu_status   = DEV_ERR_STATE;
  app_devices.baro_status  = DEV_ERR_STATE;
  app_devices.flash_status = DEV_ERR_STATE;
  app_devices.accel_status = DEV_ERR_STATE;
  App_SensorServiceReset();
  SmartLock_JournalInit();
  SmartLock_FsmInit();
  SmartLock_ButtonsInit();
  SmartLock_SecurityInit();
  SmartLock_EnvInit();
  Camera_Init();
  FrameExport_Abort();
  Evt_Init();
  Cmd_Reset();
  bench();
  s_t = 0U;
}

void tearDown(void)
{
}

/* ============================================================================ */

/* Старт єдиної прошивки: BUILD першим рядком, 8 задач, усі модулі — сильні */
static void test_boot_banner_tasks_and_strong_modules(void)
{
  static const char *const names[] = { "fsm", "buttons", "security", "env", "camera",
                                       "storage", "mem", "uart" };
  char banner[96];
  const char *o;

  o = capture(boot);
  snprintf(banner, sizeof(banner), "BUILD,fw=SmartLock,lab=LR3,git=%s,cpu_hz=1000000,loop=superloop\r\n",
           GIT_HASH);
  TEST_ASSERT_EQUAL_STRING_LEN(banner, o, strlen(banner));
  TEST_ASSERT_EQUAL(DEV_OK, app_devices.flash_status);       /* модель W25Q64 змонтовано */
  TEST_ASSERT_NOT_NULL(strstr(o, "STACK reserved="));        /* I06: пам'ять після старту */
  /* камера налаштовується при старті — AEC/AWB сходяться до першої тривоги (camera.md §8) */
  TEST_ASSERT_NOT_NULL(strstr(o, "CAM,init,status=ok,pid=0x76,ver=0x73,"));

  TEST_ASSERT_TRUE(Sched_Started());
  TEST_ASSERT_EQUAL_INT(8, Sched_Count());
  for (int i = 0; i < 8; i++)
  {
    TEST_ASSERT_EQUAL_STRING(names[i], Sched_Task(i)->name);
  }

  /* Сильні реалізації, а не слабкі заглушки smartlock_hooks_weak.c / sl_camera_link.c:
   * FSM (K02) на старті в UNLOCKED, камера (I09) приймає запит, DIAG — повний */
  TEST_ASSERT_EQUAL(SL_STATE_UNLOCKED, SmartLock_FsmState());
  TEST_ASSERT_TRUE(SmartLock_CameraRequest(77U));
  TEST_ASSERT_EQUAL_UINT32(1U, Camera_Stats()->accepted);
  TEST_ASSERT_EQUAL_UINT(0U, SmartLock_CameraStubCalls());
  Camera_Init();
  o = cmd("STATS");
  TEST_ASSERT_NULL(strstr(o, "present=0"));
  TEST_ASSERT_NOT_NULL(strstr(o, "DIAG name=fsm state=UNLOCKED "));
  TEST_ASSERT_NOT_NULL(strstr(o, "DIAG name=camera accepted=0 rejected=0 stored=0 "));
}

/* VER — той самий рядок BUILD, коли термінал відкрито вже після старту */
static void test_ver_and_help(void)
{
  char banner[96];

  (void)capture(boot);
  snprintf(banner, sizeof(banner), "BUILD,fw=SmartLock,lab=LR3,git=%s,cpu_hz=1000000,loop=superloop\r\n",
           GIT_HASH);
  TEST_ASSERT_EQUAL_STRING(banner, cmd("VER"));
  TEST_ASSERT_NOT_NULL(strstr(cmd("HELP"), "| VER |"));
  TEST_ASSERT_NOT_NULL(strstr(cmd("HELP"), "CAM LOAD <frame_id>"));
}

/* Повний сценарій на одній прошивці: LOCKED → PIR → тривога → кадр → Flash → CAM LOAD */
static void test_locked_motion_gives_photo_in_flash_and_clean_stats(void)
{
  const char *o;
  const char *p;
  char exp[96];
  uint32_t motion_id;

  (void)capture(boot);
  fake_platform.inputs &= ~PLATFORM_IN_DOOR;                  /* двері зачинені */
  s_t = fake_platform.now_ms + 10U;   /* після старту: App_Init() вже «витратив» час на затримки */
  TEST_ASSERT_TRUE(SmartLock_PostEvent(SL_EV_LOCK_REQUEST, 0U));
  (void)loop_ms(16000U);                                      /* відлік EXIT_DELAY → LOCKED */
  TEST_ASSERT_EQUAL(SL_STATE_LOCKED, SmartLock_FsmState());

  TEST_ASSERT_TRUE(SmartLock_PostEvent(SL_EV_MOTION, 0U));    /* PIR у LOCKED */
  o = loop_ms(500U);
  p = strstr(o, ",name=MOTION,arg=0,flash=ok\r\n");
  TEST_ASSERT_NOT_NULL(p);
  while ((p > o) && (strncmp(p, "EVT,id=", 7U) != 0))
  {
    p--;
  }
  motion_id = (uint32_t)strtoul(p + 7, NULL, 10);
  TEST_ASSERT_NOT_EQUAL(0U, motion_id);
  TEST_ASSERT_NOT_NULL(strstr(o, " name=alarm_triggered\r\n"));
  snprintf(exp, sizeof(exp), "CAM,photo,event=%lu,status=ok,cap=ok,", (unsigned long)motion_id);
  TEST_ASSERT_NOT_NULL(strstr(o, exp));
  snprintf(exp, sizeof(exp), ",name=PHOTO,arg=1,ref=%lu,flash=ok\r\n", (unsigned long)motion_id);
  TEST_ASSERT_NOT_NULL(strstr(o, exp));

  /* кадр 1 у W25Q64 належить саме цій події */
  snprintf(exp, sizeof(exp), "CAM,load,frame=1,status=ok,event=%lu,w=160,h=120,len=38400,",
           (unsigned long)motion_id);
  TEST_ASSERT_NOT_NULL(strstr(cmd("CAM LOAD 1"), exp));

  o = cmd("STATS");
  TEST_ASSERT_NOT_NULL(strstr(o, "STAT name=flash_frame n=1 "));
  TEST_ASSERT_NOT_NULL(strstr(o, "DIAG name=fsm state=LOCKED ignored=0 "));
  TEST_ASSERT_NOT_NULL(strstr(o, "DIAG name=camlink requested=1 rejected=0 ok=1 failed=0 timeouts=0 "
                                 "unknown=0 no_id=0 pending=0\r\n"));
  TEST_ASSERT_NOT_NULL(strstr(o, "DIAG name=camera accepted=1 rejected=0 stored=1 capture_err=0 "
                                 "store_err=0 pending=0\r\n"));
  TEST_ASSERT_NOT_NULL(strstr(o, " unsaved=0 dropped=0 "));    /* журнал: усі записи у Flash */
  TEST_ASSERT_NOT_NULL(strstr(o, "DIAG name=buttons dropped=0\r\n"));
  TEST_ASSERT_NOT_NULL(strstr(o, " dropped=0\r\nUART rx_overrun=0 cmd_overflow=0 cmd_dropped=0 "));   /* EVQ */
  TEST_ASSERT_NOT_NULL(strstr(o, "STATS_END\r\n"));
}

/* Година роботи Super Loop (360 000 проходів): час монотонний, черги без втрат */
static void test_one_hour_of_super_loop(void)
{
  uint64_t t0;
  uint64_t t1;
  uint32_t loops0;

  (void)capture(boot);
  t0 = Perf_TimeUs64();
  loops0 = Sched_Loops();
  s_t = fake_platform.now_ms + 10U;   /* після старту: App_Init() вже «витратив» час на затримки */
  (void)loop_ms(3600U * 1000U);
  t1 = Perf_TimeUs64();

  TEST_ASSERT_EQUAL_UINT32(360000U, Sched_Loops() - loops0);
  TEST_ASSERT_TRUE(t1 > t0);
  /* модельний час: 3600 с ± 1 с (Platform_Cycles на ПК — 1 МГц, переповнення 32 біт через 71 хв) */
  TEST_ASSERT_UINT64_WITHIN(1000000ULL, 3600000000ULL, t1 - t0);
  TEST_ASSERT_EQUAL(SL_STATE_UNLOCKED, SmartLock_FsmState());
  TEST_ASSERT_EQUAL_UINT32(0U, SmartLock_EventsDropped());
  TEST_ASSERT_EQUAL_UINT32(0U, SmartLock_JournalDropped());
  TEST_ASSERT_EQUAL_UINT32(0U, SmartLock_JournalUnsaved());
  TEST_ASSERT_EQUAL_UINT32(0U, SmartLock_ButtonsDropped());
  TEST_ASSERT_EQUAL_UINT32(0U, SlSecurity_EventsLost());
  TEST_ASSERT_EQUAL_UINT32(0U, Evt_Dropped());
  TEST_ASSERT_TRUE(Sched_CpuStat()->count >= 3590U);           /* вікна CPU Load ≈ 1 с */
}

int main(void)
{
  UNITY_BEGIN();
  RUN_TEST(test_boot_banner_tasks_and_strong_modules);
  RUN_TEST(test_ver_and_help);
  RUN_TEST(test_locked_motion_gives_photo_in_flash_and_clean_stats);
  RUN_TEST(test_one_hour_of_super_loop);
  return UNITY_END();
}
