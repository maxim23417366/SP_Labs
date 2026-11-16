/**
  * @file    test_frames.c
  * @brief   Кадри на рівні App/ (frames.c): FrameStore_GetOps(), стендові команди
  *          p, o і зв'язок кадру з подією журналу.
  *
  *          Флешка — fake_w25q64 (модель W25Q64 у RAM) через справжній драйвер.
  *          «Перезапуск» — повторні Storage_Init() і Frames_Init() з тим самим
  *          вмістом Flash. Вивід перехоплюється, як у test_app.c.
  */
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "unity.h"
#include "app.h"
#include "bench.h"
#include "fake_delay.h"
#include "fake_platform.h"
#include "fake_w25q64.h"
#include "flash_layout.h"
#include "frames.h"
#include "sl_journal.h"
#include "storage.h"

/* ---- перехоплення stdout --------------------------------------------------------- */
static char out[1 << 20];

static const char *capture(void (*fn)(void))
{
  FILE *tmp = tmpfile();
  int saved;
  size_t n = 0U;

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

static const char *cmd_chars;

static void run_cmds(void)
{
  for (const char *p = cmd_chars; *p != '\0'; p++)
  {
    Bench_Command(*p);
  }
}

static const char *cmd(const char *chars)
{
  cmd_chars = chars;
  return capture(run_cmds);
}

static void boot(void)
{
  Storage_Init();
  Frames_Init();
}

static void flash_present(void)
{
  app_devices.flash_status = w25q64_init(&app_devices.flash, fake_w25q64_bus(), fake_delay_ms);
  TEST_ASSERT_EQUAL(DEV_OK, app_devices.flash_status);
}

void setUp(void)
{
  fake_platform_reset();
  fake_w25q64_reset();
  fake_delay_reset();
  memset(&app_devices, 0, sizeof(app_devices));
  app_devices.flash_status = DEV_ERR_STATE;
  SmartLock_JournalInit();
}

void tearDown(void)
{
  TEST_ASSERT_EQUAL_UINT(0U, fake_w25q64_protocol_errors());
}

/* ---- без флешки -------------------------------------------------------------------- */

static void test_without_flash_frames_are_off(void)
{
  frame_header_t h;
  uint32_t id = 0U;
  uint8_t b = 0U;
  const char *o = capture(boot);

  TEST_ASSERT_NOT_NULL(strstr(o, "FRM,MOUNT,err=state,note=flash_absent\r\n"));
  TEST_ASSERT_FALSE(Frames_Ready());

  memset(&h, 0, sizeof(h));
  h.data_len = 1U;
  TEST_ASSERT_EQUAL(DEV_ERR_STATE, FrameStore_GetOps()->write_frame(&h, &b, &id));
  TEST_ASSERT_EQUAL(DEV_ERR_STATE, FrameStore_GetOps()->read_frame(1U, &h, &b, 1U));

  o = cmd("po");
  TEST_ASSERT_EQUAL_STRING("FRM,cmd=p,err=state\r\nFRM,cmd=o,err=state\r\n", o);
  TEST_ASSERT_FALSE(Frames_BenchCommand('q'));
}

static void test_mount_error_reported(void)
{
  const char *o;

  flash_present();
  fake_w25q64_fail_transfers(1U, DEV_ERR_BUS);
  o = capture(Frames_Init);
  TEST_ASSERT_EQUAL_STRING("FRM,MOUNT,err=bus\r\n", o);
  TEST_ASSERT_FALSE(Frames_Ready());
}

static void test_first_mount_line(void)
{
  const char *o;

  flash_present();
  o = capture(boot);
  TEST_ASSERT_NOT_NULL(strstr(o, "FRM,MOUNT,frames=0,next_id=1,us="));
  TEST_ASSERT_TRUE(Frames_Ready());
}

/* ---- кадри ------------------------------------------------------------------------------ */

static void test_bench_frame_write_verify_restart(void)
{
  const char *o;

  flash_present();
  (void)capture(boot);                   /* BOOT seq 1 */

  o = cmd("o");
  TEST_ASSERT_EQUAL_STRING("FRM,verify=SKIP,note=no_frames\r\n", o);

  o = cmd("p");
  TEST_ASSERT_NOT_NULL(strstr(o, "LOG,seq=2,boot=1,t_ms=0,type=TEST,arg=1,arg2=0,frame=none,crc=ok\r\n"));
  TEST_ASSERT_NOT_NULL(strstr(o, "LOG,seq=3,boot=1,t_ms=0,type=PHOTO_SAVED,arg=2,arg2=0,frame=1,crc=ok\r\n"));
  TEST_ASSERT_NOT_NULL(strstr(o, "FRM,write=PASS,id=1,event_seq=2,bytes=38400,erase_us="));

  /* Перезапуск: кадр і зв'язок з подією на місці */
  o = capture(boot);
  TEST_ASSERT_NOT_NULL(strstr(o, "FRM,MOUNT,frames=1,next_id=2,"));
  o = cmd("o");
  TEST_ASSERT_NOT_NULL(strstr(o, "FRM,verify=PASS,id=1,event_seq=2,bytes=38400,mismatch=0,crc=ok,us="));

  /* Пошкоджений байт даних: і побайтна звірка, і CRC це бачать */
  fake_w25q64_mem()[FLASH_FRAME_BASE + FLASH_FRAME_DATA_OFFSET + 100U] &= 0x00U;
  o = cmd("o");
  TEST_ASSERT_NOT_NULL(strstr(o, "FRM,verify=FAIL,id=1,event_seq=2,bytes=38400,mismatch=1,crc=bad,"));
}

static void test_bench_frame_failures(void)
{
  const char *o;

  flash_present();
  (void)capture(boot);
  fake_w25q64_fail_from(9U, 1U, DEV_ERR_BUS);     /* 9 передач — запис TEST у журнал, далі збій на стиранні слота */
  o = cmd("p");
  TEST_ASSERT_NOT_NULL(strstr(o, "type=PHOTO_FAILED,arg=2,arg2=3,frame=none"));
  TEST_ASSERT_NOT_NULL(strstr(o, "FRM,write=FAIL,id=1,event_seq=2,err=bus\r\n"));

  (void)cmd("p");                                 /* кадр 1 */
  fake_w25q64_fail_transfers(1U, DEV_ERR_BUS);
  o = cmd("o");
  TEST_ASSERT_EQUAL_STRING("FRM,verify=FAIL,id=1,err=bus\r\n", o);
}

static void test_ops_roundtrip(void)
{
  static uint8_t img[1000];
  static uint8_t rd[1000];
  frame_header_t h;
  frame_header_t got;
  uint32_t id = 0U;

  flash_present();
  (void)capture(boot);

  for (unsigned i = 0U; i < sizeof(img); i++)
  {
    img[i] = (uint8_t)(i * 13U);
  }
  memset(&h, 0, sizeof(h));
  h.format = FRAME_FORMAT_Y8;
  h.width = 40U;
  h.height = 25U;
  h.event_seq = 77U;
  h.data_len = sizeof(img);
  TEST_ASSERT_EQUAL(DEV_OK, FrameStore_GetOps()->write_frame(&h, img, &id));
  TEST_ASSERT_EQUAL_UINT32(1U, id);
  TEST_ASSERT_EQUAL(DEV_OK, FrameStore_GetOps()->read_frame(id, &got, rd, sizeof(rd)));
  TEST_ASSERT_EQUAL_MEMORY(img, rd, sizeof(img));
  TEST_ASSERT_EQUAL_UINT32(77U, got.event_seq);
  TEST_ASSERT_EQUAL_PTR(Frames_Store(), Frames_Store());
  TEST_ASSERT_EQUAL_UINT8(Frames_TestByte(1U, 0U), (uint8_t)(1U * 37U));
}

int main(void)
{
  UNITY_BEGIN();
  RUN_TEST(test_without_flash_frames_are_off);
  RUN_TEST(test_mount_error_reported);
  RUN_TEST(test_first_mount_line);
  RUN_TEST(test_bench_frame_write_verify_restart);
  RUN_TEST(test_bench_frame_failures);
  RUN_TEST(test_ops_roundtrip);
  return UNITY_END();
}
