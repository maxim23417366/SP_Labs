/**
  * @file    test_frame_export.c
  * @brief   ЛР3 (I08): формування кадру і експорт на ПК (frame_export.c,
  *          команди CAM PATTERN / CAM EXPORT у cam_test.c).
  *
  *          Останній тест проганяє експорт синтетичного кадру через задачу uart
  *          Super Loop і зберігає весь вивід у build/frame_export_host.log —
  *          ціль `make tools` відновлює з нього кадр скриптом tools/frame_decode.py
  *          і порівнює побайтно з тестовим малюнком (C → лог → Python → PNG).
  */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "unity.h"
#include "app.h"
#include "app_config.h"
#include "app_tasks.h"
#include "cam_test.h"
#include "cmd.h"
#include "crc32.h"
#include "evt.h"
#include "fake_camera.h"
#include "fake_i2c.h"
#include "fake_platform.h"
#include "frame_export.h"
#include "ov7670.h"
#include "sched.h"
#include "sl_buttons.h"
#include "sl_env.h"
#include "sl_fsm.h"
#include "sl_journal.h"
#include "sl_security.h"

#define HOST_LOG "build/frame_export_host.log"

static char out[160000];
static const char *line_cmd;

static void run_line(void)
{
  Cmd_Execute(line_cmd);
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

static void run_steps_to_end(void)
{
  while (FrameExport_Step())
  {
  }
}

static unsigned count(const char *s, const char *needle)
{
  unsigned n = 0U;

  for (const char *p = strstr(s, needle); p != NULL; p = strstr(p + 1, needle))
  {
    n++;
  }
  return n;
}

/* Розбирає FRM-рядки назад у байти — так, як це робить tools/frame_decode.py */
static uint32_t decode(const char *log, uint8_t *dst, uint32_t cap)
{
  uint32_t total = 0U;

  for (const char *p = strstr(log, "FRM off="); p != NULL; p = strstr(p + 1, "FRM off="))
  {
    char *end;
    uint32_t off = (uint32_t)strtoul(p + 8, &end, 10);
    const char *h = strstr(end, "d=") + 2;

    for (; (h[0] != '\r') && (h[0] != '\0'); h += 2)
    {
      char byte[3] = { h[0], h[1], '\0' };

      if (off < cap)
      {
        dst[off] = (uint8_t)strtoul(byte, NULL, 16);
      }
      off++;
      total++;
    }
  }
  return total;
}

void setUp(void)
{
  fake_platform_reset();
  fake_i2c_reset();
  fake_camera_reset();
  /* Super Loop без сенсорів і Flash: модулі sl_* бачать «пристрій відсутній» */
  memset(&app_devices, 0, sizeof(app_devices));
  app_devices.mpu_status   = DEV_ERR_NACK;
  app_devices.baro_status  = DEV_ERR_NACK;
  app_devices.flash_status = DEV_ERR_NACK;
  app_devices.accel_status = DEV_ERR_NACK;
  SmartLock_JournalInit();
  SmartLock_FsmInit();
  SmartLock_ButtonsInit();
  SmartLock_SecurityInit();
  SmartLock_EnvInit();
  AppTasks_Init();
  CamTest_Reset();
  FrameExport_Abort();
  Evt_Init();
}

void tearDown(void)
{
  FrameExport_Abort();
}

/* Рядок FRM: зсув, hex великими літерами, межа буфера */
static void test_format_chunk(void)
{
  const uint8_t d[3] = { 0x00U, 0xABU, 0x5FU };
  char buf[32];

  TEST_ASSERT_EQUAL_INT(19, FrameExport_FormatChunk(buf, sizeof(buf), 64U, d, 3U));
  TEST_ASSERT_EQUAL_STRING("FRM off=64 d=00AB5F", buf);
  TEST_ASSERT_EQUAL_INT(-1, FrameExport_FormatChunk(buf, 19U, 64U, d, 3U));   /* немає місця на '\0' */
  TEST_ASSERT_EQUAL_INT(-1, FrameExport_FormatChunk(NULL, sizeof(buf), 0U, d, 3U));
  TEST_ASSERT_EQUAL_INT(-1, FrameExport_FormatChunk(buf, sizeof(buf), 0U, NULL, 3U));
}

/* Невеликий буфер: FRAME_BEGIN → FRM по одному за крок → FRAME_END; CRC = crc32_compute */
static uint8_t s_small[70];

static void run_small_export(void)
{
  TEST_ASSERT_TRUE(FrameExport_Start(s_small, sizeof(s_small), 7U));
  TEST_ASSERT_TRUE(FrameExport_Busy());
  TEST_ASSERT_FALSE(FrameExport_Start(s_small, sizeof(s_small), 8U));   /* уже йде */
  TEST_ASSERT_TRUE(FrameExport_Step());     /* FRM off=0  (32 Б) */
  TEST_ASSERT_TRUE(FrameExport_Step());     /* FRM off=32 (32 Б) */
  TEST_ASSERT_TRUE(FrameExport_Step());     /* FRM off=64 (6 Б)  */
  TEST_ASSERT_TRUE(FrameExport_Busy());
  TEST_ASSERT_TRUE(FrameExport_Step());     /* FRAME_END */
  TEST_ASSERT_FALSE(FrameExport_Busy());
  TEST_ASSERT_FALSE(FrameExport_Step());
}

static void run_small_start_abort(void)
{
  TEST_ASSERT_TRUE(FrameExport_Start(s_small, sizeof(s_small), 9U));
  FrameExport_Abort();
}

static void test_start_step_end_and_crc(void)
{
  char exp[160];
  const char *o;
  uint32_t crc;

  for (unsigned i = 0U; i < sizeof(s_small); i++)
  {
    s_small[i] = (uint8_t)(i * 7U);
  }
  crc = crc32_compute(s_small, sizeof(s_small));
  TEST_ASSERT_FALSE(FrameExport_Step());          /* нічого не експортується */
  TEST_ASSERT_FALSE(FrameExport_Start(NULL, 10U, 1U));
  TEST_ASSERT_FALSE(FrameExport_Start(s_small, 0U, 1U));

  o = capture(run_small_export);
  snprintf(exp, sizeof(exp),
           "FRAME_BEGIN id=7 seq=0 w=160 h=120 fmt=RGB565 len=70 crc32=%08lX chunk=32\r\n",
           (unsigned long)crc);
  TEST_ASSERT_EQUAL_STRING_LEN(exp, o, strlen(exp));
  TEST_ASSERT_NOT_NULL(strstr(o, "\r\nFRM off=0 d=00070E15"));
  TEST_ASSERT_NOT_NULL(strstr(o, "\r\nFRM off=32 d="));
  TEST_ASSERT_NOT_NULL(strstr(o, "\r\nFRM off=64 d=C0C7CED5DCE3\r\n"));
  snprintf(exp, sizeof(exp), "FRAME_END id=7 len=70 crc32=%08lX lines=3\r\n", (unsigned long)crc);
  TEST_ASSERT_NOT_NULL(strstr(o, exp));
  TEST_ASSERT_EQUAL_UINT(3U, count(o, "FRM off="));
  TEST_ASSERT_EQUAL_UINT(2U, Evt_Pending());      /* export_start / export_done */

  /* Abort: експорт можна почати знову (вивід FRAME_BEGIN тут не перевіряється) */
  o = capture(run_small_start_abort);
  TEST_ASSERT_NOT_NULL(strstr(o, "FRAME_BEGIN id=9 "));
  TEST_ASSERT_FALSE(FrameExport_Busy());
}

/* Тестовий малюнок: ті самі контрольні пікселі, що в tools/test_frame_decode.py */
static void test_pattern_reference_pixels(void)
{
  static uint8_t f[CAMERA_FRAME_BYTES];

  memset(f, 0x55, sizeof(f));
  CamTest_FillTestPattern(f, (uint16_t)CAMERA_WIDTH, (uint16_t)CAMERA_HEIGHT);
  TEST_ASSERT_EQUAL_HEX8(0xFFU, f[0]);                       /* біла смуга */
  TEST_ASSERT_EQUAL_HEX8(0xFFU, f[1]);
  TEST_ASSERT_EQUAL_HEX8(0xFFU, f[40]);                      /* x = 20: жовта смуга 0xFFE0 */
  TEST_ASSERT_EQUAL_HEX8(0xE0U, f[41]);
  TEST_ASSERT_EQUAL_HEX8(0x00U, f[(60U * 160U) * 2U]);       /* чорний початок градієнта */
  TEST_ASSERT_EQUAL_HEX8(0xFFU, f[sizeof(f) - 1U]);          /* білий кінець градієнта */
  CamTest_FillTestPattern(NULL, 160U, 120U);                 /* без падіння */
  CamTest_FillTestPattern(f, 1U, 1U);
  TEST_ASSERT_EQUAL_HEX8(0xFFU, f[0]);                       /* width < 2 — буфер не чіпається */
}

/* CAM EXPORT без кадру; CAM PATTERN → CAM EXPORT; поки йде експорт — CAM,busy */
static void test_cam_pattern_export_and_busy(void)
{
  const char *o;
  size_t bytes = 0U;

  TEST_ASSERT_NOT_NULL(strstr(cmd("CAM EXPORT"), "CAM,export,status=no_frame\r\n"));

  o = cmd("CAM PATTERN");
  TEST_ASSERT_NOT_NULL(strstr(o, "CAM,pattern,bytes=38400,crc=0x"));
  (void)CamTest_Frame(&bytes);
  TEST_ASSERT_EQUAL_UINT(CAMERA_FRAME_BYTES, bytes);

  o = cmd("CAM EXPORT");
  TEST_ASSERT_NOT_NULL(strstr(o, "FRAME_BEGIN id=0 seq=0 w=160 h=120 fmt=RGB565 len=38400 crc32="));
  TEST_ASSERT_TRUE(FrameExport_Busy());

  /* буфер заблоковано: ні знімок, ні малюнок його не перезаписують */
  TEST_ASSERT_NOT_NULL(strstr(cmd("CAM TEST"), "CAM,busy,op=test,reason=export\r\n"));
  TEST_ASSERT_NOT_NULL(strstr(cmd("CAM PATTERN"), "CAM,busy,op=pattern,reason=export\r\n"));
  TEST_ASSERT_NOT_NULL(strstr(cmd("CAM EXPORT"), "CAM,busy,op=export,reason=export\r\n"));
  TEST_ASSERT_EQUAL_UINT32(0U, fake_camera.calls);

  o = capture(run_steps_to_end);
  TEST_ASSERT_EQUAL_UINT(1200U, count(o, "FRM off="));
  TEST_ASSERT_NOT_NULL(strstr(o, "FRAME_END id=0 len=38400 crc32="));
  TEST_ASSERT_NOT_NULL(strstr(o, " lines=1200\r\n"));
  TEST_ASSERT_FALSE(FrameExport_Busy());

  TEST_ASSERT_NOT_NULL(strstr(cmd("HELP"), "CAM PATTERN (test frame) | CAM EXPORT"));
}

/* Кадр з камери (CAM TEST) теж експортується: CRC у CAM,result = crc32 у FRAME_BEGIN */
static void test_export_after_cam_test_keeps_same_crc(void)
{
  uint8_t *r = fake_i2c_add_device(OV7670_I2C_ADDR);
  const char *o;
  const char *p;
  char crc_line[16];
  char crc_hdr[16];

  r[OV7670_REG_PID] = OV7670_PID_VALUE;
  r[OV7670_REG_VER] = OV7670_VER_VALUE;
  o = cmd("CAM TEST");
  p = strstr(o, "crc=0x");
  TEST_ASSERT_NOT_NULL(p);
  memcpy(crc_line, p + 6, 8U);
  crc_line[8] = '\0';

  o = cmd("CAM EXPORT");
  p = strstr(o, "crc32=");
  TEST_ASSERT_NOT_NULL(p);
  memcpy(crc_hdr, p + 6, 8U);
  crc_hdr[8] = '\0';
  TEST_ASSERT_EQUAL_STRING(crc_line, crc_hdr);
  FrameExport_Abort();
}

/* Повний шлях у Super Loop: CAM PATTERN + CAM EXPORT з UART → задача uart друкує
 * по рядку за прохід, між ними EVT/LOAD → відновлення байтів = вихідний кадр.
 * Вивід зберігається в build/frame_export_host.log для tools/frame_decode.py. */
static void run_super_loop_export(void)
{
  fake_platform.uart_rx = "CAM PATTERN\rCAM EXPORT\r";
  fake_platform.uart_pos = 0U;
  for (unsigned pass = 0U; pass < 3000U; pass++)
  {
    fake_platform.now_ms += 10U;
    Sched_RunOnce();
    if ((pass > 10U) && !FrameExport_Busy())
    {
      break;
    }
  }
  for (unsigned pass = 0U; pass < 5U; pass++)   /* задача uart дописує EVT export_done */
  {
    fake_platform.now_ms += 10U;
    Sched_RunOnce();
  }
}

static void test_super_loop_export_roundtrip_and_host_log(void)
{
  static uint8_t ref[CAMERA_FRAME_BYTES];
  static uint8_t got[CAMERA_FRAME_BYTES];
  const char *o;
  FILE *f;
  unsigned frm_before_end;

  CamTest_FillTestPattern(ref, (uint16_t)CAMERA_WIDTH, (uint16_t)CAMERA_HEIGHT);
  o = capture(run_super_loop_export);
  TEST_ASSERT_FALSE(FrameExport_Busy());
  TEST_ASSERT_NOT_NULL(strstr(o, "CAM,pattern,bytes=38400"));
  TEST_ASSERT_NOT_NULL(strstr(o, "FRAME_BEGIN id=0 "));
  TEST_ASSERT_NOT_NULL(strstr(o, "FRAME_END id=0 len=38400 "));
  TEST_ASSERT_NOT_NULL(strstr(o, " name=export_start_0\r\n"));
  TEST_ASSERT_NOT_NULL(strstr(o, " name=export_done_0\r\n"));

  /* рядки не розірвані: кожен FRM починається з початку рядка */
  frm_before_end = count(o, "\r\nFRM off=");
  TEST_ASSERT_EQUAL_UINT(1200U, frm_before_end);

  memset(got, 0, sizeof(got));
  TEST_ASSERT_EQUAL_UINT32(CAMERA_FRAME_BYTES, decode(o, got, sizeof(got)));
  TEST_ASSERT_EQUAL_MEMORY(ref, got, sizeof(ref));

  f = fopen(HOST_LOG, "wb");
  TEST_ASSERT_NOT_NULL(f);
  (void)fputs("BUILD,fw=SmartLock,lab=LR3,git=host,cpu_hz=1000000,loop=superloop\r\n", f);
  (void)fwrite(o, 1U, strlen(o), f);
  (void)fclose(f);
}

int main(void)
{
  UNITY_BEGIN();
  RUN_TEST(test_format_chunk);
  RUN_TEST(test_start_step_end_and_crc);
  RUN_TEST(test_pattern_reference_pixels);
  RUN_TEST(test_cam_pattern_export_and_busy);
  RUN_TEST(test_export_after_cam_test_keeps_same_crc);
  RUN_TEST(test_super_loop_export_roundtrip_and_host_log);
  return UNITY_END();
}
