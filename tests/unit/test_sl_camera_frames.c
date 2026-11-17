/**
  * @file    test_sl_camera_frames.c
  * @brief   ЛР3 (K08/K09): кадр охоронної події → frames.c (H10) → W25Q64 →
  *          PHOTO / PHOTO_ERR у журналі (sl_camera_frames.c, sl_camera_link.c).
  *
  *          Справжні модулі: sl_journal → storage.c (H09) і frames.c / frame_store
  *          (H10) поверх fake_w25q64 (модель W25Q64 у RAM) через справжній драйвер.
  *          Модуля камери (I09) ще немає: «камера» — слабка заглушка
  *          SmartLock_CameraRequest() (приймає запит), кадр — буфер тесту.
  *          Перевіряється зв'язок в обидва боки: заголовок кадру → seq події і
  *          запис PHOTO_SAVED → seq події та frame_id; після «рестарту» — те саме.
  */
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "unity.h"
#include "app.h"
#include "fake_delay.h"
#include "fake_platform.h"
#include "fake_w25q64.h"
#include "flash_log.h"
#include "frames.h"
#include "sl_camera_frames.h"
#include "sl_camera_link.h"
#include "sl_fsm.h"
#include "sl_journal.h"
#include "smartlock_hooks.h"
#include "storage.h"

/* ---- перехоплення stdout --------------------------------------------------------- */
static char out[1 << 16];

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

static void boot(void)
{
  Storage_Init();
  Frames_Init();
}

static void drain(void)
{
  while (SmartLock_StoragePoll(Platform_Millis()))
  {
  }
}

/* Знайти у Flash дійсний запис журналу з цим seq */
static bool flash_find(uint32_t seq, flash_log_entry_t *e)
{
  flash_log_iter_t it;
  flash_log_slot_t state;

  flash_log_iter_begin(Storage_Journal(), &it);
  while ((flash_log_next(Storage_Journal(), &it, e, &state, NULL) == DEV_OK) &&
         (state != FLASH_LOG_SLOT_EMPTY))
  {
    if ((state == FLASH_LOG_SLOT_VALID) && (e->seq == seq))
    {
      return true;
    }
  }
  return false;
}

static uint8_t img[2000];

static void flash_present(void)
{
  app_devices.flash_status = w25q64_init(&app_devices.flash, fake_w25q64_bus(), fake_delay_ms);
  TEST_ASSERT_EQUAL(DEV_OK, app_devices.flash_status);
}

/* Охоронна подія з прийнятим запитом кадру — як FSM у LOCKED */
static uint32_t security_event(sl_log_type_t type)
{
  uint32_t id = SmartLock_LogEvent(type, 0U);

  TEST_ASSERT_NOT_EQUAL(SL_LOG_ID_NONE, id);
  TEST_ASSERT_TRUE(SmartLock_CameraLinkStart(id, Platform_Millis()));
  TEST_ASSERT_TRUE(SmartLock_CameraLinkAwaiting(id));
  return id;
}

void setUp(void)
{
  fake_platform_reset();
  fake_w25q64_reset();
  fake_delay_reset();
  memset(&app_devices, 0, sizeof(app_devices));
  app_devices.flash_status = DEV_ERR_STATE;
  SmartLock_JournalInit();
  SmartLock_FsmInit();               /* скидає і список запитів до камери */
  SmartLock_CameraStubReset();
  SmartLock_CameraStubSetAccept(true);
  for (unsigned i = 0U; i < sizeof(img); i++)
  {
    img[i] = (uint8_t)(i * 7U + 3U);
  }
}

void tearDown(void)
{
  TEST_ASSERT_EQUAL_UINT(0U, fake_w25q64_protocol_errors());
}

/* ============================================================================ */

static void test_frame_saved_and_linked_both_ways_survives_restart(void)
{
  static uint8_t rd[sizeof(img)];
  flash_log_entry_t e;
  frame_header_t h;
  uint32_t id;
  uint32_t photo_seq;
  const char *o;

  flash_present();
  (void)capture(boot);                                    /* BOOT = seq 1 */
  fake_platform.now_ms = 500U;
  id = security_event(SL_LOG_MOTION);
  TEST_ASSERT_EQUAL_UINT32(2U, id);

  TEST_ASSERT_EQUAL(DEV_OK, SmartLock_CameraStoreFrame(id, FRAME_FORMAT_Y8, 50U, 40U, img, sizeof(img)));
  TEST_ASSERT_FALSE(SmartLock_CameraLinkAwaiting(id));
  TEST_ASSERT_EQUAL_UINT(0U, SmartLock_CameraLinkPending());
  TEST_ASSERT_EQUAL_UINT32(1U, SmartLock_CameraLinkStats()->ok);
  photo_seq = SmartLock_JournalLastSeq();
  o = capture(drain);
  TEST_ASSERT_NOT_NULL(strstr(o, "EVT,id=2,t_ms=500,name=MOTION,arg=0,flash=ok\r\n"));
  TEST_ASSERT_NOT_NULL(strstr(o, "EVT,id=3,t_ms=500,name=PHOTO,arg=1,ref=2,flash=ok\r\n"));

  /* журнал → кадр: PHOTO_SAVED з arg = seq події і frame_id */
  TEST_ASSERT_TRUE(flash_find(photo_seq, &e));
  TEST_ASSERT_EQUAL_HEX8(FLASH_LOG_PHOTO_SAVED, e.type);
  TEST_ASSERT_EQUAL_UINT32(id, e.arg);
  TEST_ASSERT_EQUAL_UINT32(1U, e.frame_id);
  TEST_ASSERT_TRUE(flash_find(id, &e));
  TEST_ASSERT_EQUAL_HEX8(FLASH_LOG_MOTION, e.type);

  /* кадр → журнал: заголовок зберігає seq події, дані цілі */
  TEST_ASSERT_EQUAL(DEV_OK, FrameStore_GetOps()->read_frame(1U, &h, rd, sizeof(rd)));
  TEST_ASSERT_EQUAL_UINT32(id, h.event_seq);
  TEST_ASSERT_EQUAL_UINT8(FRAME_FORMAT_Y8, h.format);
  TEST_ASSERT_EQUAL_UINT16(50U, h.width);
  TEST_ASSERT_EQUAL_MEMORY(img, rd, sizeof(img));

  /* «рестарт»: RAM скинуто, той самий вміст Flash */
  SmartLock_JournalInit();
  SmartLock_FsmInit();
  (void)capture(boot);
  TEST_ASSERT_TRUE(flash_find(photo_seq, &e));
  TEST_ASSERT_EQUAL_UINT32(id, e.arg);
  TEST_ASSERT_EQUAL(DEV_OK, FrameStore_GetOps()->read_frame(e.frame_id, &h, rd, sizeof(rd)));
  TEST_ASSERT_EQUAL_UINT32(id, h.event_seq);
  /* нумерація продовжилась: BOOT другого запуску = 4, наступна подія = 5 */
  TEST_ASSERT_EQUAL_UINT32(5U, SmartLock_LogEvent(SL_LOG_TAMPER, 0U));
}

static void test_store_failure_gives_photo_failed_with_reason(void)
{
  flash_log_entry_t e;
  uint32_t id;

  flash_present();
  (void)capture(boot);
  id = security_event(SL_LOG_TAMPER);
  fake_w25q64_fail_transfers(1U, DEV_ERR_BUS);           /* збій на стиранні слота кадру */
  TEST_ASSERT_EQUAL(DEV_ERR_BUS, SmartLock_CameraStoreFrame(id, FRAME_FORMAT_Y8, 50U, 40U, img, sizeof(img)));
  TEST_ASSERT_EQUAL_UINT32(1U, SmartLock_CameraLinkStats()->failed);
  (void)capture(drain);
  TEST_ASSERT_TRUE(flash_find(id + 1U, &e));
  TEST_ASSERT_EQUAL_HEX8(FLASH_LOG_PHOTO_FAILED, e.type);
  TEST_ASSERT_EQUAL_UINT32(id, e.arg);                   /* seq події */
  TEST_ASSERT_EQUAL_UINT32(SL_CAM_ERR_STORE, e.arg2);    /* причина — sl_cam_status_t */
  TEST_ASSERT_EQUAL_UINT32(FLASH_LOG_NO_FRAME, e.frame_id);
}

static void test_frames_unavailable_closes_request_as_store_error(void)
{
  sl_log_rec_t r;
  uint32_t id;

  (void)capture(boot);                                    /* флешки немає */
  id = security_event(SL_LOG_MOTION);
  TEST_ASSERT_EQUAL(DEV_ERR_STATE, SmartLock_CameraStoreFrame(id, FRAME_FORMAT_Y8, 50U, 40U, img, sizeof(img)));
  TEST_ASSERT_TRUE(SmartLock_JournalPeek(1U, &r));
  TEST_ASSERT_EQUAL_UINT8(SL_LOG_PHOTO_ERR, r.type);
  TEST_ASSERT_EQUAL_UINT32(SL_CAM_ERR_STORE, r.arg);
  TEST_ASSERT_EQUAL_UINT32(id, r.ref);
}

static void test_bad_buffer_is_capture_error(void)
{
  sl_log_rec_t r;
  uint32_t a;
  uint32_t b;
  unsigned programs;

  flash_present();
  (void)capture(boot);
  a = security_event(SL_LOG_MOTION);
  b = security_event(SL_LOG_TAMPER);
  programs = fake_w25q64_program_count();
  TEST_ASSERT_EQUAL(DEV_ERR_PARAM, SmartLock_CameraStoreFrame(a, FRAME_FORMAT_Y8, 1U, 1U, NULL, 1U));
  TEST_ASSERT_EQUAL(DEV_ERR_PARAM, SmartLock_CameraStoreFrame(b, FRAME_FORMAT_Y8, 1U, 1U, img, 0U));
  TEST_ASSERT_TRUE(SmartLock_JournalPeek(2U, &r));
  TEST_ASSERT_EQUAL_UINT8(SL_LOG_PHOTO_ERR, r.type);
  TEST_ASSERT_EQUAL_UINT32(SL_CAM_ERR_CAPTURE, r.arg);
  TEST_ASSERT_EQUAL_UINT32(a, r.ref);
  TEST_ASSERT_EQUAL_UINT32(2U, SmartLock_CameraLinkStats()->failed);
  TEST_ASSERT_EQUAL_UINT(programs, fake_w25q64_program_count());   /* у Flash нічого не писали */
}

/* Кадр після тайм-ауту або для чужого ID у Flash не пишеться: кадру без посилання немає */
static void test_late_or_unknown_frame_is_not_written(void)
{
  uint32_t id;
  unsigned programs;

  flash_present();
  (void)capture(boot);
  fake_platform.now_ms = 1000U;
  id = security_event(SL_LOG_MOTION);
  TEST_ASSERT_TRUE(SmartLock_CameraLinkPoll(1000U + TBD_CAM_RESULT_TIMEOUT_MS));   /* PHOTO_ERR TIMEOUT */
  TEST_ASSERT_FALSE(SmartLock_CameraLinkAwaiting(id));
  programs = fake_w25q64_program_count();

  TEST_ASSERT_EQUAL(DEV_ERR_STATE, SmartLock_CameraStoreFrame(id, FRAME_FORMAT_Y8, 50U, 40U, img, sizeof(img)));
  TEST_ASSERT_EQUAL(DEV_ERR_STATE, SmartLock_CameraStoreFrame(id + 77U, FRAME_FORMAT_Y8, 50U, 40U, img, sizeof(img)));
  TEST_ASSERT_EQUAL(DEV_ERR_STATE, SmartLock_CameraStoreFrame(SL_LOG_ID_NONE, FRAME_FORMAT_Y8, 50U, 40U, img, sizeof(img)));
  TEST_ASSERT_EQUAL_UINT(programs, fake_w25q64_program_count());
  TEST_ASSERT_EQUAL_UINT32(1U, Frames_Store()->next_id);              /* жодного кадру */
  TEST_ASSERT_EQUAL_UINT32(3U, SmartLock_CameraLinkStats()->unknown);
  TEST_ASSERT_EQUAL_UINT32(1U, SmartLock_CameraLinkStats()->timeouts);
}

int main(void)
{
  UNITY_BEGIN();
  RUN_TEST(test_frame_saved_and_linked_both_ways_survives_restart);
  RUN_TEST(test_store_failure_gives_photo_failed_with_reason);
  RUN_TEST(test_frames_unavailable_closes_request_as_store_error);
  RUN_TEST(test_bad_buffer_is_capture_error);
  RUN_TEST(test_late_or_unknown_frame_is_not_written);
  return UNITY_END();
}
