/**
  * @file    test_cam_test.c
  * @brief   ЛР3 (I07): команда CAM TEST (cam_test.c) — ініціалізація OV7670 по SCCB
  *          і один кадр через Platform_CameraCapture().
  *
  *          Камера — регістри fake_i2c за адресою 0x21 (SCCB у fake_platform),
  *          захоплення — fake_camera (кадр із байтів (uint8_t)i). Сам алгоритм
  *          захоплення по DVP перевіряє test_camera_dvp.c.
  */
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "unity.h"
#include "app_config.h"
#include "app_tasks.h"
#include "cam_test.h"
#include "cmd.h"
#include "crc32.h"
#include "evt.h"
#include "fake_camera.h"
#include "fake_i2c.h"
#include "fake_platform.h"
#include "ov7670.h"
#include "stats.h"

static char out[16384];
static const char *line_cmd;

static void run_line(void)
{
  Cmd_Execute(line_cmd);
}

static void run_flush(void)
{
  (void)Evt_Flush(8U);
}

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

static const char *cmd(const char *line)
{
  line_cmd = line;
  return capture(run_line);
}

static uint8_t *add_camera(void)
{
  uint8_t *r = fake_i2c_add_device(OV7670_I2C_ADDR);

  r[OV7670_REG_PID] = OV7670_PID_VALUE;
  r[OV7670_REG_VER] = OV7670_VER_VALUE;
  return r;
}

void setUp(void)
{
  fake_platform_reset();
  fake_i2c_reset();
  fake_camera_reset();
  AppTasks_Init();                /* таблиця задач і LAT — для виводу STATS */
  CamTest_Reset();
  CamTest_ResetStats();
  Evt_Init();
}

void tearDown(void)
{
}

/* Камера на місці: CAM,init ok, кадр 38 400 Б, EVT capture_start/done, STAT camera */
static void test_cam_test_inits_and_captures_one_frame(void)
{
  uint8_t *r = add_camera();
  const char *o;
  char exp[160];
  size_t bytes = 0U;
  const uint8_t *frame;

  o = cmd("CAM TEST");
  TEST_ASSERT_NOT_NULL(strstr(o, "CAM,init,status=ok,pid=0x76,ver=0x73,clkrc=0x0F,"
                                 "failed_reg=0x00\r\n"));
  TEST_ASSERT_EQUAL_HEX8(TBD_CAMERA_CLKRC, r[OV7670_REG_CLKRC]);   /* CLKRC записано */

  frame = CamTest_Frame(&bytes);
  TEST_ASSERT_EQUAL_UINT(CAMERA_FRAME_BYTES, bytes);
  TEST_ASSERT_EQUAL_HEX8(0x05U, frame[5]);
  snprintf(exp, sizeof(exp),
           "CAM,result,status=ok,bytes=38400,lines=120,short=0,vglitch=0,capture_us=530000,"
           "crc=0x%08lX\r\n", (unsigned long)crc32_compute(frame, bytes));
  TEST_ASSERT_NOT_NULL(strstr(o, exp));

  /* параметри захоплення — з app_config.h (docs/camera.md §3–4) */
  TEST_ASSERT_EQUAL_UINT32(1U, fake_camera.calls);
  TEST_ASSERT_EQUAL_UINT16(CAMERA_WIDTH, fake_camera.last_cfg.width);
  TEST_ASSERT_EQUAL_UINT16(CAMERA_HEIGHT, fake_camera.last_cfg.height);
  TEST_ASSERT_EQUAL_UINT8(CAMERA_BPP, fake_camera.last_cfg.bytes_per_pixel);
  TEST_ASSERT_EQUAL_UINT32(TBD_CAMERA_VSYNC_MIN_US, fake_camera.last_cfg.vsync_min_us);
  TEST_ASSERT_EQUAL_UINT(CAMERA_FRAME_BYTES, fake_camera.last_cap);

  /* metrics-spec §3.3: capture_start → capture_done */
  TEST_ASSERT_EQUAL_UINT(2U, Evt_Pending());
  o = capture(run_flush);
  TEST_ASSERT_NOT_NULL(strstr(o, " name=capture_start\r\n"));
  TEST_ASSERT_NOT_NULL(strstr(o, " name=capture_done\r\n"));

  /* metrics-spec §3.2: тривалість — STAT name=camera */
  TEST_ASSERT_EQUAL_UINT32(1U, CamTest_Stat()->count);
  o = cmd("STATS");
  TEST_ASSERT_NOT_NULL(strstr(o, "STAT name=camera n=1 min_us=530000 avg_us=530000 max_us=530000\r\n"));
  (void)cmd("STATS RESET");
  TEST_ASSERT_EQUAL_UINT32(0U, CamTest_Stat()->count);

  /* повторний CAM TEST не ініціалізує камеру вдруге */
  o = cmd("CAM TEST");
  TEST_ASSERT_NULL(strstr(o, "CAM,init"));
  TEST_ASSERT_NOT_NULL(strstr(o, "CAM,result,status=ok,"));
  TEST_ASSERT_EQUAL_UINT32(2U, fake_camera.calls);
}

/* Камери немає / не та: кадр не знімається, причина — у CAM,init */
static void test_cam_test_reports_missing_or_wrong_camera(void)
{
  const char *o = cmd("CAM TEST");

  TEST_ASSERT_NOT_NULL(strstr(o, "CAM,init,status=nack,pid=0x00,ver=0x00,"));
  TEST_ASSERT_NULL(strstr(o, "CAM,result"));
  TEST_ASSERT_EQUAL_UINT32(0U, fake_camera.calls);

  add_camera()[OV7670_REG_PID] = 0x77U;
  o = cmd("CAM TEST");
  TEST_ASSERT_NOT_NULL(strstr(o, "CAM,init,status=wrong_id,pid=0x77,"));
  TEST_ASSERT_EQUAL_UINT32(0U, fake_camera.calls);
}

/* SCCB не відповідає вчасно: статус timeout, кадру немає. Зупинку посеред таблиці
 * (failed_reg) перевіряє test_ov7670.c::test_configure_stops_on_failed_register. */
static void test_cam_test_reports_failed_register(void)
{
  const char *o;

  add_camera();
  fake_platform.sccb_status = DEV_ERR_TIMEOUT;
  o = cmd("CAM TEST");
  TEST_ASSERT_NOT_NULL(strstr(o, "CAM,init,status=timeout,"));
  TEST_ASSERT_EQUAL_UINT32(0U, fake_camera.calls);

  fake_platform.sccb_status = DEV_OK;          /* камера ожила — наступний CAM TEST */
  o = cmd("CAM TEST");
  TEST_ASSERT_NOT_NULL(strstr(o, "CAM,init,status=ok,"));
  TEST_ASSERT_EQUAL_UINT32(1U, fake_camera.calls);
}

/* Помилки захоплення — назва статусу з docs/camera.md §4; STAT лише для вдалих */
static void test_cam_test_capture_errors(void)
{
  const char *o;

  add_camera();
  fake_camera.status = PLATFORM_CAM_ERR_VSYNC_TIMEOUT;
  fake_camera.cycles = 1500000U;
  o = cmd("CAM TEST");
  TEST_ASSERT_NOT_NULL(strstr(o, "CAM,result,status=vsync_timeout,bytes=0,lines=0,"));
  TEST_ASSERT_EQUAL_UINT32(0U, CamTest_Stat()->count);

  fake_camera.status = PLATFORM_CAM_ERR_SHORT_FRAME;
  fake_camera.short_lines = 3U;
  fake_camera.vsync_glitches = 2U;
  o = cmd("CAM TEST");
  TEST_ASSERT_NOT_NULL(strstr(o, "status=short_frame,bytes=38400,lines=120,short=3,vglitch=2,"));

  fake_camera.status = PLATFORM_CAM_ERR_HREF_TIMEOUT;
  TEST_ASSERT_NOT_NULL(strstr(cmd("CAM TEST"), "status=href_timeout,"));
  fake_camera.status = PLATFORM_CAM_ERR_PCLK_TIMEOUT;
  TEST_ASSERT_NOT_NULL(strstr(cmd("CAM TEST"), "status=pclk_timeout,"));
  fake_camera.status = PLATFORM_CAM_ERR_OVERFLOW;
  TEST_ASSERT_NOT_NULL(strstr(cmd("CAM TEST"), "status=overflow,"));
  fake_camera.status = PLATFORM_CAM_ERR_ARG;
  TEST_ASSERT_NOT_NULL(strstr(cmd("CAM TEST"), "status=arg,"));
  TEST_ASSERT_EQUAL_UINT32(0U, CamTest_Stat()->count);
}

/* Розбір рядка і довідка */
static void test_cam_test_line_parsing_and_help(void)
{
  size_t bytes = 1U;

  TEST_ASSERT_FALSE(CamTest_LineCommand(NULL));
  TEST_ASSERT_FALSE(CamTest_LineCommand("CAM"));
  TEST_ASSERT_FALSE(CamTest_LineCommand("CAM TESTX"));
  TEST_ASSERT_NOT_NULL(CamTest_Frame(NULL));
  (void)CamTest_Frame(&bytes);
  TEST_ASSERT_EQUAL_UINT(0U, bytes);
  TEST_ASSERT_NOT_NULL(strstr(cmd("HELP"), "CAM TEST (OV7670: init + 1 frame QQVGA RGB565)"));
}

int main(void)
{
  UNITY_BEGIN();
  RUN_TEST(test_cam_test_inits_and_captures_one_frame);
  RUN_TEST(test_cam_test_reports_missing_or_wrong_camera);
  RUN_TEST(test_cam_test_reports_failed_register);
  RUN_TEST(test_cam_test_capture_errors);
  RUN_TEST(test_cam_test_line_parsing_and_help);
  return UNITY_END();
}
