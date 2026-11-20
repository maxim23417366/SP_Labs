/**
  * @file    test_camera.c
  * @brief   ЛР3 (I09): кадр охоронної події → W25Q64 → назад, з прив'язкою до ID події.
  *
  *          Справжні модулі: camera.c (сильна SmartLock_CameraRequest), cam_test.c,
  *          sl_camera_link / sl_camera_frames (K08), sl_journal → storage.c (H09),
  *          frames.c / frame_store (H10) поверх fake_w25q64 (модель W25Q64 у RAM)
  *          через справжній драйвер w25q64.c. Підмінено лише захоплення по DVP
  *          (fake_camera — кадр із байтів (uint8_t)i) і SCCB (fake_i2c, 0x21).
  *
  *          Окремий бінарник (tests/Makefile, APP_OBJS_camera): у решті тестів App/
  *          лишається слабка заглушка камери, на яку спираються тести K08.
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
#include "camera.h"
#include "cmd.h"
#include "event_queue.h"
#include "evt.h"
#include "fake_camera.h"
#include "fake_delay.h"
#include "fake_i2c.h"
#include "fake_platform.h"
#include "fake_w25q64.h"
#include "flash_log.h"
#include "frame_export.h"
#include "frames.h"
#include "ov7670.h"
#include "sched.h"
#include "sl_buttons.h"
#include "sl_camera_link.h"
#include "sl_env.h"
#include "sl_fsm.h"
#include "sl_journal.h"
#include "sl_security.h"
#include "smartlock_hooks.h"
#include "storage.h"

static char out[1 << 17];
static const char *line_cmd;
static uint32_t poll_now;

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

static void run_line(void)    { Cmd_Execute(line_cmd); }
static void run_poll(void)    { (void)SmartLock_CameraPoll(poll_now); }
static void run_boot(void)    { Storage_Init(); Frames_Init(); }
static void run_drain(void)
{
  while (SmartLock_StoragePoll(Platform_Millis()))
  {
  }
}
static void run_flush(void)   { (void)Evt_Flush(16U); }

static const char *cmd(const char *line)
{
  line_cmd = line;
  return capture(run_line);
}

static void flash_present(void)
{
  app_devices.flash_status = w25q64_init(&app_devices.flash, fake_w25q64_bus(), fake_delay_ms);
  TEST_ASSERT_EQUAL(DEV_OK, app_devices.flash_status);
}

static void camera_present(void)
{
  uint8_t *r = fake_i2c_add_device(OV7670_I2C_ADDR);

  r[OV7670_REG_PID] = OV7670_PID_VALUE;
  r[OV7670_REG_VER] = OV7670_VER_VALUE;
}

/* Охоронна подія, як її записує FSM у LOCKED: подія в журнал → запит кадру */
static uint32_t security_event(sl_log_type_t type)
{
  uint32_t id = SmartLock_LogEvent(type, 0U);

  TEST_ASSERT_NOT_EQUAL(SL_LOG_ID_NONE, id);
  return id;
}

void setUp(void)
{
  fake_platform_reset();
  fake_i2c_reset();
  fake_w25q64_reset();
  fake_delay_reset();
  fake_camera_reset();
  memset(&app_devices, 0, sizeof(app_devices));
  app_devices.flash_status = DEV_ERR_STATE;
  app_devices.mpu_status   = DEV_ERR_NACK;
  app_devices.baro_status  = DEV_ERR_NACK;
  app_devices.accel_status = DEV_ERR_NACK;
  SmartLock_JournalInit();
  SmartLock_FsmInit();               /* скидає і список запитів sl_camera_link */
  SmartLock_ButtonsInit();
  SmartLock_SecurityInit();
  SmartLock_EnvInit();
  AppTasks_Init();
  CamTest_Reset();
  CamTest_ResetStats();
  Camera_Init();
  FrameExport_Abort();
  Evt_Init();
  poll_now = 0U;
}

void tearDown(void)
{
  FrameExport_Abort();
  TEST_ASSERT_EQUAL_UINT(0U, fake_w25q64_protocol_errors());
}

/* ============================================================================ */

/* Правила прийому запиту: один за раз, не під час експорту, не для ID = 0 */
static void test_request_rules(void)
{
  TEST_ASSERT_FALSE(SmartLock_CameraRequest(0U));
  TEST_ASSERT_TRUE(SmartLock_CameraRequest(5U));
  TEST_ASSERT_TRUE(Camera_Pending());
  TEST_ASSERT_FALSE(SmartLock_CameraRequest(6U));        /* зайнята попереднім кадром */
  TEST_ASSERT_EQUAL_UINT32(1U, Camera_Stats()->accepted);
  TEST_ASSERT_EQUAL_UINT32(2U, Camera_Stats()->rejected);

  Camera_Init();
  (void)cmd("CAM PATTERN");
  (void)cmd("CAM EXPORT");                               /* буфер кадру передається на ПК */
  TEST_ASSERT_TRUE(FrameExport_Busy());
  TEST_ASSERT_FALSE(SmartLock_CameraRequest(7U));
  TEST_ASSERT_FALSE(Camera_Pending());
  TEST_ASSERT_FALSE(SmartLock_CameraPoll(0U));           /* без запиту задача нічого не робить */
}

/* Головний критерій I09: write_frame / read_frame побайтно і прив'язка до ID події
 * в обидва боки (заголовок кадру → seq події, PHOTO_SAVED → seq і frame_id) */
static void test_alarm_frame_written_and_read_back_bytewise(void)
{
  static uint8_t rd[CAMERA_FRAME_BYTES];
  frame_header_t h;
  flash_log_entry_t e;
  flash_log_iter_t it;
  flash_log_slot_t state;
  const uint8_t *frame;
  size_t bytes = 0U;
  uint32_t id;
  uint32_t frame_id;
  bool found = false;
  const char *o;

  flash_present();
  camera_present();
  (void)capture(run_boot);                                   /* BOOT = seq 1 */
  fake_platform.now_ms = 500U;
  id = security_event(SL_LOG_MOTION);
  TEST_ASSERT_TRUE(SmartLock_CameraLinkStart(id, 500U));     /* → сильна SmartLock_CameraRequest */
  TEST_ASSERT_TRUE(Camera_Pending());
  TEST_ASSERT_TRUE(SmartLock_CameraLinkAwaiting(id));

  poll_now = 500U;
  o = capture(run_poll);                                     /* задача camera: кадр → Flash */
  TEST_ASSERT_NOT_NULL(strstr(o, "CAM,init,status=ok,"));
  TEST_ASSERT_NOT_NULL(strstr(o, "CAM,photo,event=2,status=ok,cap=ok,capture_us=530000,store_us="));
  TEST_ASSERT_FALSE(Camera_Pending());
  TEST_ASSERT_FALSE(SmartLock_CameraLinkAwaiting(id));       /* запит закрито рівно один раз */
  TEST_ASSERT_EQUAL_UINT32(1U, SmartLock_CameraLinkStats()->ok);
  TEST_ASSERT_EQUAL_UINT32(1U, Camera_Stats()->stored);
  TEST_ASSERT_EQUAL_UINT32(1U, CamTest_StoreStat()->count);

  o = capture(run_drain);
  TEST_ASSERT_NOT_NULL(strstr(o, "EVT,id=2,t_ms=500,name=MOTION,arg=0,flash=ok\r\n"));
  /* PHOTO: arg = ID кадру у Flash, ref = ID події (журнал K07/K08) */
  TEST_ASSERT_NOT_NULL(strstr(o, ",name=PHOTO,arg=1,ref=2,flash=ok\r\n"));
  frame_id = 1U;

  /* кадр → подія: заголовок у Flash має event_seq = ID події; дані побайтно */
  frame = CamTest_Frame(&bytes);
  TEST_ASSERT_EQUAL_UINT(CAMERA_FRAME_BYTES, bytes);
  memset(rd, 0, sizeof(rd));
  TEST_ASSERT_EQUAL(DEV_OK, FrameStore_GetOps()->read_frame(frame_id, &h, rd, sizeof(rd)));
  TEST_ASSERT_EQUAL_UINT32(id, h.event_seq);
  TEST_ASSERT_EQUAL_UINT8(FRAME_FORMAT_RGB565, h.format);
  TEST_ASSERT_EQUAL_UINT16(CAMERA_WIDTH, h.width);
  TEST_ASSERT_EQUAL_UINT16(CAMERA_HEIGHT, h.height);
  TEST_ASSERT_EQUAL_UINT32(CAMERA_FRAME_BYTES, h.data_len);
  TEST_ASSERT_EQUAL_MEMORY(frame, rd, CAMERA_FRAME_BYTES);
  for (uint32_t i = 0U; i < CAMERA_FRAME_BYTES; i += 997U)
  {
    /* те, що «зняла камера» (байт i = i), з переставленими байтами пікселя:
     * у Flash RGB565 — старший байт першим (CAMERA_RGB565_SWAP) */
    TEST_ASSERT_EQUAL_HEX8((uint8_t)(i ^ 1U), rd[i]);
  }

  /* подія → кадр: запис PHOTO_SAVED у Flash посилається на seq події і frame_id */
  flash_log_iter_begin(Storage_Journal(), &it);
  while ((flash_log_next(Storage_Journal(), &it, &e, &state, NULL) == DEV_OK) &&
         (state != FLASH_LOG_SLOT_EMPTY))
  {
    if ((state == FLASH_LOG_SLOT_VALID) && (e.type == FLASH_LOG_PHOTO_SAVED))
    {
      TEST_ASSERT_EQUAL_UINT32(id, e.arg);
      TEST_ASSERT_EQUAL_UINT32(frame_id, e.frame_id);
      found = true;
    }
  }
  TEST_ASSERT_TRUE(found);

  /* metrics-spec §3.3: capture_* і flash_wr_* навколо зйомки та запису */
  o = capture(run_flush);
  TEST_ASSERT_NOT_NULL(strstr(o, " name=capture_start\r\n"));
  TEST_ASSERT_NOT_NULL(strstr(o, " name=capture_done\r\n"));
  TEST_ASSERT_NOT_NULL(strstr(o, " name=flash_wr_start\r\n"));
  TEST_ASSERT_NOT_NULL(strstr(o, " name=flash_wr_done\r\n"));
  TEST_ASSERT_NOT_NULL(strstr(cmd("STATS"), "STAT name=flash_frame n=1 "));
  /* PB2 — захоплення, PB12 — запис кадру (docs/superloop.md §4) */
  TEST_ASSERT_EQUAL_UINT32(1U, fake_platform.probe_rises[PLATFORM_PROBE_CAMERA]);
  TEST_ASSERT_EQUAL_UINT32(1U, fake_platform.probe_rises[PLATFORM_PROBE_STORAGE]);
}

/* Після «рестарту» кадр читається за frame_id, CAM EXPORT передає його з id події */
static void test_cam_load_after_restart_and_export_with_event_id(void)
{
  static uint8_t saved[CAMERA_FRAME_BYTES];
  const uint8_t *frame;
  size_t bytes = 0U;
  uint32_t id;
  const char *o;
  char exp[96];

  flash_present();
  camera_present();
  (void)capture(run_boot);
  fake_platform.now_ms = 700U;
  id = security_event(SL_LOG_TAMPER);
  TEST_ASSERT_TRUE(SmartLock_CameraLinkStart(id, 700U));
  (void)capture(run_poll);
  memcpy(saved, CamTest_Frame(NULL), sizeof(saved));
  (void)capture(run_drain);

  /* «рестарт»: RAM скинуто (буфер затертий), Flash той самий */
  SmartLock_JournalInit();
  SmartLock_FsmInit();
  CamTest_Reset();
  (void)cmd("CAM PATTERN");                                 /* у буфері інший кадр */
  (void)capture(run_boot);

  o = cmd("CAM LOAD 1");
  snprintf(exp, sizeof(exp), "CAM,load,frame=1,status=ok,event=%lu,w=160,h=120,len=38400,crc=0x",
           (unsigned long)id);
  TEST_ASSERT_NOT_NULL(strstr(o, exp));
  frame = CamTest_Frame(&bytes);
  TEST_ASSERT_EQUAL_UINT(CAMERA_FRAME_BYTES, bytes);
  TEST_ASSERT_EQUAL_MEMORY(saved, frame, sizeof(saved));

  o = cmd("CAM EXPORT");
  snprintf(exp, sizeof(exp), "FRAME_BEGIN id=%lu seq=0 w=160 h=120 fmt=RGB565 len=38400 ",
           (unsigned long)id);
  TEST_ASSERT_NOT_NULL(strstr(o, exp));
  TEST_ASSERT_NOT_NULL(strstr(cmd("CAM LOAD 1"), "CAM,busy,op=load,reason=export\r\n"));
  FrameExport_Abort();

  TEST_ASSERT_NOT_NULL(strstr(cmd("CAM LOAD 9"), "CAM,load,frame=9,status=no_frame,"));
  TEST_ASSERT_NOT_NULL(strstr(cmd("CAM LOAD x"), "CAM,load,frame=0,status=param,"));
  TEST_ASSERT_NOT_NULL(strstr(cmd("CAM EXPORT"), "CAM,export,status=no_frame\r\n"));   /* буфер скинуто */
}

/* Кадр не захоплено → PHOTO_ERR CAPTURE з ref = ID події, у Flash кадру немає */
static void test_capture_error_gives_photo_err_capture(void)
{
  uint32_t id;
  const char *o;
  frame_header_t h;
  static uint8_t rd[16];

  flash_present();
  camera_present();
  (void)capture(run_boot);
  id = security_event(SL_LOG_MOTION);
  fake_camera.status = PLATFORM_CAM_ERR_VSYNC_TIMEOUT;
  TEST_ASSERT_TRUE(SmartLock_CameraLinkStart(id, 0U));
  o = capture(run_poll);
  TEST_ASSERT_NOT_NULL(strstr(o, "CAM,photo,event=2,status=capture_err,cap=vsync_timeout,"));
  TEST_ASSERT_EQUAL_UINT32(1U, Camera_Stats()->capture_err);
  TEST_ASSERT_EQUAL_UINT32(1U, SmartLock_CameraLinkStats()->failed);
  o = capture(run_drain);
  TEST_ASSERT_NOT_NULL(strstr(o, ",name=PHOTO_ERR,arg=2,ref=2,flash=ok\r\n"));   /* 2 = CAPTURE */
  TEST_ASSERT_EQUAL(DEV_ERR_ID, FrameStore_GetOps()->read_frame(1U, &h, rd, sizeof(rd)));

  /* камера не відповідає по SCCB — теж CAPTURE, без CAM,result */
  fake_i2c_reset();
  CamTest_Reset();
  id = security_event(SL_LOG_TAMPER);
  TEST_ASSERT_TRUE(SmartLock_CameraLinkStart(id, 0U));
  o = capture(run_poll);
  TEST_ASSERT_NOT_NULL(strstr(o, "CAM,init,status=nack,"));
  TEST_ASSERT_NOT_NULL(strstr(o, ",status=capture_err,cap=arg,"));
  TEST_ASSERT_EQUAL_UINT32(2U, Camera_Stats()->capture_err);
}

/* Кадр є, Flash немає → PHOTO_ERR STORE (запит закрито, повторів немає) */
static void test_store_error_gives_photo_err_store(void)
{
  uint32_t id;
  const char *o;

  camera_present();
  (void)capture(run_boot);                                   /* Flash відсутня */
  id = security_event(SL_LOG_MOTION);
  TEST_ASSERT_TRUE(SmartLock_CameraLinkStart(id, 0U));
  o = capture(run_poll);
  TEST_ASSERT_NOT_NULL(strstr(o, ",status=store_err,cap=ok,"));
  TEST_ASSERT_EQUAL_UINT32(1U, Camera_Stats()->store_err);
  TEST_ASSERT_EQUAL_UINT32(0U, CamTest_StoreStat()->count);
  TEST_ASSERT_FALSE(SmartLock_CameraLinkAwaiting(id));
  o = capture(run_drain);
  {
    char exp[48];

    snprintf(exp, sizeof(exp), ",name=PHOTO_ERR,arg=3,ref=%lu,", (unsigned long)id);   /* 3 = STORE */
    TEST_ASSERT_NOT_NULL(strstr(o, exp));
  }
}

/* Повний шлях у Super Loop: LOCKED → PIR (MOTION) → FSM → камера → Flash → PHOTO.
 * Подія і кадр мають той самий ID; інша MOTION, поки кадр не оброблено, не губиться. */
static uint32_t s_t;

static void run_loop_ms(uint32_t ms)
{
  for (uint32_t end = s_t + ms; s_t < end; s_t += 10U)
  {
    fake_platform.now_ms = s_t;
    Sched_RunOnce();
  }
}

static void run_loop_lock(void)    { run_loop_ms(16000U); }
static void run_loop_short(void)   { run_loop_ms(300U); }

static void test_super_loop_motion_in_locked_gives_linked_photo(void)
{
  static uint8_t rd[CAMERA_FRAME_BYTES];
  frame_header_t h;
  const char *o;
  const char *p;
  uint32_t motion_id;

  flash_present();
  camera_present();
  (void)capture(run_boot);
  fake_platform.inputs &= ~PLATFORM_IN_DOOR;                 /* двері зачинені */
  s_t = 100U;
  (void)SmartLock_PostEvent(SL_EV_LOCK_REQUEST, 0U);
  (void)capture(run_loop_lock);                              /* 15 с відліку → LOCKED */
  TEST_ASSERT_EQUAL(SL_STATE_LOCKED, SmartLock_FsmState());

  (void)SmartLock_PostEvent(SL_EV_MOTION, 0U);               /* PIR у LOCKED */
  o = capture(run_loop_short);
  p = strstr(o, ",name=MOTION,arg=0,flash=ok\r\n");
  TEST_ASSERT_NOT_NULL(p);
  while ((p > o) && (strncmp(p, "EVT,id=", 7U) != 0))
  {
    p--;
  }
  motion_id = (uint32_t)strtoul(p + 7, NULL, 10);
  TEST_ASSERT_NOT_EQUAL(0U, motion_id);
  TEST_ASSERT_NOT_NULL(strstr(o, "CAM,photo,event="));
  TEST_ASSERT_NOT_NULL(strstr(o, " name=alarm_triggered\r\n"));    /* FSM (K05), I04 */
  {
    char exp[64];

    snprintf(exp, sizeof(exp), ",name=PHOTO,arg=1,ref=%lu,flash=ok\r\n", (unsigned long)motion_id);
    TEST_ASSERT_NOT_NULL(strstr(o, exp));
  }
  TEST_ASSERT_EQUAL(DEV_OK, FrameStore_GetOps()->read_frame(1U, &h, rd, sizeof(rd)));
  TEST_ASSERT_EQUAL_UINT32(motion_id, h.event_seq);
  TEST_ASSERT_EQUAL_MEMORY(CamTest_Frame(NULL), rd, CAMERA_FRAME_BYTES);
  TEST_ASSERT_EQUAL_UINT(0U, SmartLock_CameraLinkPending());
  TEST_ASSERT_EQUAL_UINT32(1U, SmartLock_CameraLinkStats()->ok);
}

int main(void)
{
  UNITY_BEGIN();
  RUN_TEST(test_request_rules);
  RUN_TEST(test_alarm_frame_written_and_read_back_bytewise);
  RUN_TEST(test_cam_load_after_restart_and_export_with_event_id);
  RUN_TEST(test_capture_error_gives_photo_err_capture);
  RUN_TEST(test_store_error_gives_photo_err_store);
  RUN_TEST(test_super_loop_motion_in_locked_gives_linked_photo);
  return UNITY_END();
}
