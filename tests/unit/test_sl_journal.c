/**
  * @file    test_sl_journal.c
  * @brief   ЛР3 (K07/K08/K09): журнал подій у RAM (sl_journal.c) і зв'язок
  *          подія → фото (sl_camera_link.c) — порядок і ID, переповнення FIFO і
  *          резерв для PHOTO, задача storage (одна порція за прохід, рядок EVT
  *          у UART), відмови бекенда, відновлення нумерації через
  *          SmartLock_JournalSetLastSeq(), 100 подій без втрат (FR-08, PRD) і
  *          слабка заглушка камери.
  *
  *          Бекенд — мок «Flash»: тест визначає власну SmartLock_JournalOps()
  *          (лінкується без storage_ops.c, tests/Makefile) і складає прийняті
  *          записи в масив. Справжній бекенд W25Q64 (storage.c + fake_w25q64)
  *          перевіряють test_storage.c і test_sl_integration.c. Слабкий бекенд
  *          «Flash недоступний» перевіряє test_sl_fsm.c, а сильну камеру
  *          підставляє він же; тут камера — слабка заглушка.
  */
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "unity.h"

#include "event_queue.h"
#include "fake_platform.h"
#include "platform.h"
#include "sl_camera_link.h"
#include "sl_fsm.h"
#include "sl_journal.h"
#include "smartlock_hooks.h"

/* ---- мок бекенду ------------------------------------------------------------------ */
#define MOCK_FLASH_RECS 256U

static dev_status_t mock_status;        /* що повертає append */
static bool         mock_no_ops;        /* SmartLock_JournalOps() → NULL */
static bool         mock_no_append;     /* ops без append */
static unsigned     mock_calls;
static sl_log_rec_t mock_last;
static sl_log_rec_t mock_flash[MOCK_FLASH_RECS];   /* «Flash»: прийняті записи по порядку */
static unsigned     mock_flash_n;

static dev_status_t mock_append(const sl_log_rec_t *rec)
{
  mock_calls++;
  mock_last = *rec;
  if ((mock_status == DEV_OK) && (mock_flash_n < MOCK_FLASH_RECS))
  {
    mock_flash[mock_flash_n++] = *rec;
  }
  return mock_status;
}

const sl_journal_ops_t *SmartLock_JournalOps(void)
{
  static const sl_journal_ops_t ops       = { mock_append };
  static const sl_journal_ops_t ops_empty = { NULL };

  if (mock_no_ops)
  {
    return NULL;
  }
  return mock_no_append ? &ops_empty : &ops;
}

/* ---- перехоплення stdout (як у test_app.c) ------------------------------------------- */
static char out[4096];
static bool poll_result;

static void poll_once(void)
{
  poll_result = SmartLock_StoragePoll(Platform_Millis());
}

/* Один прохід storage з stdout у файлі; між початком і кінцем не можна TEST_ASSERT */
static const char *poll_captured(void)
{
  FILE *tmp = tmpfile();
  int saved;
  size_t n;

  out[0] = '\0';
  if (tmp == NULL) { return out; }
  fflush(stdout);
  saved = dup(1);
  dup2(fileno(tmp), 1);
  poll_once();
  fflush(stdout);
  dup2(saved, 1);
  close(saved);
  rewind(tmp);
  n = fread(out, 1U, sizeof(out) - 1U, tmp);
  out[n] = '\0';
  fclose(tmp);
  return out;
}

void setUp(void)
{
  fake_platform_reset();
  SmartLock_EventsInit();
  SmartLock_JournalInit();
  SmartLock_FsmInit();               /* скидає і список запитів до камери */
  SmartLock_CameraStubReset();
  mock_status = DEV_OK;
  mock_no_ops = false;
  mock_no_append = false;
  mock_calls = 0U;
  memset(&mock_last, 0, sizeof(mock_last));
  memset(mock_flash, 0, sizeof(mock_flash));
  mock_flash_n = 0U;
}

void tearDown(void)
{
}

/* ============================================================================ */

/* скільки звичайних записів вміщає FIFO (останні місця — резерв для PHOTO / PHOTO_ERR) */
#define NORMAL_CAP (TBD_JOURNAL_FIFO_LEN - TBD_JOURNAL_FIFO_RESERVE)

static void test_log_assigns_increasing_ids_and_time(void)
{
  sl_log_rec_t r;

  fake_platform.now_ms = 100U;
  TEST_ASSERT_EQUAL_UINT32(1U, SmartLock_LogEvent(SL_LOG_LOCK_ACTIVATED, 15000U));
  fake_platform.now_ms = 15100U;
  TEST_ASSERT_EQUAL_UINT32(2U, SmartLock_LogEvent(SL_LOG_LOCKED, 15000U));
  TEST_ASSERT_EQUAL_UINT32(3U, SmartLock_LogEventRef(SL_LOG_PHOTO, 7U, 2U));
  TEST_ASSERT_EQUAL_UINT(3U, SmartLock_JournalPending());
  TEST_ASSERT_EQUAL_UINT32(3U, SmartLock_JournalLastSeq());

  TEST_ASSERT_TRUE(SmartLock_JournalPeek(0U, &r));
  TEST_ASSERT_EQUAL_UINT32(1U, r.seq);
  TEST_ASSERT_EQUAL_UINT32(100U, r.t_ms);
  TEST_ASSERT_EQUAL_UINT8(SL_LOG_LOCK_ACTIVATED, r.type);
  TEST_ASSERT_EQUAL_UINT32(15000U, r.arg);
  TEST_ASSERT_EQUAL_UINT32(0U, r.ref);
  TEST_ASSERT_TRUE(SmartLock_JournalPeek(2U, &r));
  TEST_ASSERT_EQUAL_UINT8(SL_LOG_PHOTO, r.type);
  TEST_ASSERT_EQUAL_UINT32(7U, r.arg);
  TEST_ASSERT_EQUAL_UINT32(2U, r.ref);
  TEST_ASSERT_EQUAL_UINT32(15100U, r.t_ms);
  TEST_ASSERT_FALSE(SmartLock_JournalPeek(3U, &r));
  TEST_ASSERT_FALSE(SmartLock_JournalPeek(0U, NULL));
}

static void test_invalid_type_is_rejected_without_consuming_id(void)
{
  TEST_ASSERT_EQUAL_UINT32(SL_LOG_ID_NONE, SmartLock_LogEvent(SL_LOG_NONE, 0U));
  TEST_ASSERT_EQUAL_UINT32(SL_LOG_ID_NONE, SmartLock_LogEvent(SL_LOG_COUNT, 0U));
  TEST_ASSERT_EQUAL_UINT(0U, SmartLock_JournalPending());
  TEST_ASSERT_EQUAL_UINT32(1U, SmartLock_LogEvent(SL_LOG_MOTION, 0U));
}

static void test_overflow_drops_new_counts_and_leaves_id_gap(void)
{
  sl_log_rec_t r;

  for (unsigned i = 0U; i < NORMAL_CAP; i++)
  {
    TEST_ASSERT_EQUAL_UINT32(i + 1U, SmartLock_LogEvent(SL_LOG_MOTION, i));
  }
  TEST_ASSERT_EQUAL_UINT32(SL_LOG_ID_NONE, SmartLock_LogEvent(SL_LOG_TAMPER, 99U));   /* звичайні події: місць немає */
  TEST_ASSERT_EQUAL_UINT32(1U, SmartLock_JournalDropped());
  TEST_ASSERT_EQUAL_UINT(NORMAL_CAP, SmartLock_JournalPending());
  TEST_ASSERT_EQUAL_UINT(NORMAL_CAP, SmartLock_JournalMaxDepth());

  /* найстаріші записи не перезаписано */
  TEST_ASSERT_TRUE(SmartLock_JournalPeek(0U, &r));
  TEST_ASSERT_EQUAL_UINT32(1U, r.seq);

  /* звільнили місце: новий запис отримує ID після втраченого — пропуск видно */
  (void)poll_captured();
  TEST_ASSERT_EQUAL_UINT32(NORMAL_CAP + 2U, SmartLock_LogEvent(SL_LOG_TAMPER, 1U));
  TEST_ASSERT_TRUE(SmartLock_JournalPeek(NORMAL_CAP - 1U, &r));
  TEST_ASSERT_EQUAL_UINT32(NORMAL_CAP + 2U, r.seq);
}

static void test_storage_poll_one_record_per_pass_in_order(void)
{
  const char *o;

  TEST_ASSERT_FALSE(SmartLock_StoragePoll(0U));   /* порожньо — немає корисної роботи */
  fake_platform.now_ms = 42U;
  (void)SmartLock_LogEvent(SL_LOG_WRONG_CODE, 4U);
  (void)SmartLock_LogEvent(SL_LOG_ACCESS_GRANTED, 4U);
  (void)SmartLock_LogEventRef(SL_LOG_PHOTO, 9U, 1U);

  o = poll_captured();
  TEST_ASSERT_TRUE(poll_result);
  TEST_ASSERT_EQUAL_STRING("EVT,id=1,t_ms=42,name=WRONG_CODE,arg=4,flash=ok\r\n", o);
  TEST_ASSERT_EQUAL_UINT(2U, SmartLock_JournalPending());
  TEST_ASSERT_EQUAL_UINT(1U, mock_calls);
  TEST_ASSERT_EQUAL_UINT32(1U, mock_last.seq);
  TEST_ASSERT_EQUAL_UINT8(SL_LOG_WRONG_CODE, mock_last.type);

  o = poll_captured();
  TEST_ASSERT_EQUAL_STRING("EVT,id=2,t_ms=42,name=ACCESS_GRANTED,arg=4,flash=ok\r\n", o);
  o = poll_captured();
  TEST_ASSERT_EQUAL_STRING("EVT,id=3,t_ms=42,name=PHOTO,arg=9,ref=1,flash=ok\r\n", o);
  TEST_ASSERT_EQUAL_UINT32(3U, SmartLock_JournalStored());
  TEST_ASSERT_EQUAL_UINT32(0U, SmartLock_JournalUnsaved());
  o = poll_captured();
  TEST_ASSERT_FALSE(poll_result);
  TEST_ASSERT_EQUAL_STRING("", o);
}

static void test_backend_failures_count_unsaved_and_do_not_block(void)
{
  const char *o;

  mock_status = DEV_ERR_STATE;   /* журнал вимкнено (storage.c) — запис не збережено, без повторів */
  (void)SmartLock_LogEvent(SL_LOG_FORCED_OPEN, 0U);
  o = poll_captured();
  TEST_ASSERT_EQUAL_STRING("EVT,id=1,t_ms=0,name=FORCED_OPEN,arg=0,flash=err\r\n", o);
  TEST_ASSERT_EQUAL_UINT32(1U, SmartLock_JournalUnsaved());
  TEST_ASSERT_EQUAL_UINT(0U, SmartLock_JournalPending());   /* запис не застряг у FIFO */

  /* Помилка шини / тайм-аут стирання — так само одна спроба: повтор стирання робить
   * storage.c наступним записом, а невдалий запис уже витратив seq у Flash */
  mock_status = DEV_ERR_TIMEOUT;
  (void)SmartLock_LogEvent(SL_LOG_MOTION, 1U);
  o = poll_captured();
  TEST_ASSERT_EQUAL_STRING("EVT,id=2,t_ms=0,name=MOTION,arg=1,flash=err\r\n", o);
  TEST_ASSERT_EQUAL_UINT(2U, mock_calls);

  mock_no_append = true;
  (void)SmartLock_LogEvent(SL_LOG_ENV_WARNING, 3U);
  (void)poll_captured();
  mock_no_ops = true;
  (void)SmartLock_LogEvent(SL_LOG_DOOR_OPEN, 0U);
  (void)poll_captured();
  TEST_ASSERT_EQUAL_UINT32(4U, SmartLock_JournalUnsaved());
  TEST_ASSERT_EQUAL_UINT32(0U, SmartLock_JournalStored());
  TEST_ASSERT_EQUAL_UINT(2U, mock_calls);
}

/* Мертва Flash не гальмує UART-журнал: кожен прохід — рядок, черга не росте */
static void test_dead_flash_keeps_uart_journal_running(void)
{
  unsigned printed = 0U;

  mock_status = DEV_ERR_TIMEOUT;
  for (unsigned i = 0U; i < 400U; i++)
  {
    fake_platform.now_ms += 10U;
    (void)SmartLock_LogEvent(SL_LOG_MOTION, i);
    (void)SmartLock_LogEvent(SL_LOG_TAMPER, i);   /* дві події на один прохід storage */
    if (strstr(poll_captured(), "flash=err") != NULL)
    {
      printed++;
    }
    TEST_ASSERT_TRUE(SmartLock_JournalPending() <= TBD_JOURNAL_FIFO_LEN);
  }
  TEST_ASSERT_TRUE(SmartLock_JournalMaxDepth() <= TBD_JOURNAL_FIFO_LEN);
  TEST_ASSERT_TRUE(SmartLock_JournalDropped() > 0U);   /* подій більше, ніж проходів */
  TEST_ASSERT_EQUAL_UINT(400U, printed);                /* UART-журнал не зупинявся жодного проходу */
  TEST_ASSERT_EQUAL_UINT32(printed, SmartLock_JournalUnsaved());
  TEST_ASSERT_EQUAL_UINT32(0U, SmartLock_JournalStored());
}

static void test_fifo_wraps_around(void)
{
  sl_log_rec_t r;

  /* кілька разів довжина FIFO: запис → вилучення, індекси переходять через край */
  for (unsigned i = 0U; i < (3U * TBD_JOURNAL_FIFO_LEN); i++)
  {
    (void)SmartLock_LogEvent(SL_LOG_DOOR_CLOSED, i);
    (void)poll_captured();
  }
  (void)SmartLock_LogEvent(SL_LOG_DOOR_OPEN, 1U);
  (void)SmartLock_LogEvent(SL_LOG_DOOR_CLOSED, 2U);
  TEST_ASSERT_TRUE(SmartLock_JournalPeek(1U, &r));
  TEST_ASSERT_EQUAL_UINT32((3U * TBD_JOURNAL_FIFO_LEN) + 2U, r.seq);
  TEST_ASSERT_EQUAL_UINT32(2U, r.arg);
  TEST_ASSERT_EQUAL_UINT(2U, SmartLock_JournalMaxDepth());
}

static void test_restored_sequence_and_wrap_skip_zero(void)
{
  SmartLock_JournalSetLastSeq(41U);   /* H09: останній seq з Flash після перезапуску */
  TEST_ASSERT_EQUAL_UINT32(42U, SmartLock_LogEvent(SL_LOG_LOCKED, 0U));
  SmartLock_JournalSetLastSeq(0xFFFFFFFFU);
  TEST_ASSERT_EQUAL_UINT32(1U, SmartLock_LogEvent(SL_LOG_UNLOCKED, 0U));   /* 0 = «немає запису» */
}

static void test_type_names_match_prd(void)
{
  static const char *const names[SL_LOG_COUNT] =
  {
    "NONE", "LOCK_ACTIVATED", "LOCKED", "UNLOCKED", "ACCESS_GRANTED", "WRONG_CODE",
    "MOTION", "TAMPER", "FORCED_OPEN", "ENV_WARNING", "DOOR_OPEN", "DOOR_CLOSED", "PHOTO", "PHOTO_ERR",
  };

  for (unsigned t = 0U; t < (unsigned)SL_LOG_COUNT; t++)
  {
    TEST_ASSERT_EQUAL_STRING(names[t], SmartLock_LogTypeName((sl_log_type_t)t));
  }
  TEST_ASSERT_EQUAL_STRING("?", SmartLock_LogTypeName(SL_LOG_COUNT));
}

/* ---- резерв FIFO для PHOTO / PHOTO_ERR ------------------------------------------------ */
static void test_reserve_slots_are_only_for_photo_records(void)
{
  for (unsigned i = 0U; i < NORMAL_CAP; i++)
  {
    (void)SmartLock_LogEvent(SL_LOG_ENV_WARNING, i);
  }
  TEST_ASSERT_EQUAL_UINT32(SL_LOG_ID_NONE, SmartLock_LogEvent(SL_LOG_MOTION, 0U));
  TEST_ASSERT_EQUAL_UINT32(SL_LOG_ID_NONE, SmartLock_LogEvent(SL_LOG_UNLOCKED, 0U));
  for (unsigned i = 0U; i < TBD_JOURNAL_FIFO_RESERVE; i++)
  {
    TEST_ASSERT_NOT_EQUAL(SL_LOG_ID_NONE, SmartLock_LogEventRef((i == 0U) ? SL_LOG_PHOTO : SL_LOG_PHOTO_ERR, 1U, 1U));
  }
  TEST_ASSERT_EQUAL_UINT(TBD_JOURNAL_FIFO_LEN, SmartLock_JournalPending());
  TEST_ASSERT_EQUAL_UINT32(SL_LOG_ID_NONE, SmartLock_LogEventRef(SL_LOG_PHOTO, 1U, 1U));   /* повна і з резервом */
  TEST_ASSERT_EQUAL_UINT32(3U, SmartLock_JournalDropped());
}

/* ---- відновлення нумерації після перезапуску (E2E-06) ---------------------------------- */
/* storage.c після монтування передає останній seq з Flash; виклик посеред роботи
 * (після записів BOOT / стендових поза чергою) теж зсуває нумерацію далі */
static void test_set_last_seq_continues_numbering_mid_run(void)
{
  sl_log_rec_t r;

  TEST_ASSERT_EQUAL_UINT32(1U, SmartLock_LogEvent(SL_LOG_MOTION, 0U));
  SmartLock_JournalSetLastSeq(100U);
  TEST_ASSERT_EQUAL_UINT32(101U, SmartLock_LogEvent(SL_LOG_TAMPER, 0U));
  TEST_ASSERT_EQUAL_UINT32(101U, SmartLock_JournalLastSeq());
  TEST_ASSERT_TRUE(SmartLock_JournalPeek(0U, &r));
  TEST_ASSERT_EQUAL_UINT32(1U, r.seq);   /* уже виданий ID у черзі не змінюється */
  TEST_ASSERT_TRUE(SmartLock_JournalPeek(1U, &r));
  TEST_ASSERT_EQUAL_UINT32(101U, r.seq);
}

/* ---- конвеєр FSM → журнал → «Flash» ------------------------------------------------------ */
static void post_ev(sl_event_type_t t, uint32_t arg)
{
  TEST_ASSERT_TRUE(SmartLock_PostEvent(t, arg));
}

static void unpack_all(sl_log_rec_t *recs, unsigned n)
{
  TEST_ASSERT_EQUAL_UINT(n, mock_flash_n);
  memcpy(recs, mock_flash, n * sizeof(recs[0]));
}

/* PRD FR-08: 100 подій — усі збережені, у порядку, з монотонним ID і часом; потім «перезапуск» */
static void test_hundred_events_through_pipeline_without_loss(void)
{
  static sl_log_rec_t recs[101];
  unsigned n = 0U;
  uint32_t t = 0U;

  for (unsigned round = 0U; round < 25U; round++)
  {
    for (unsigned k = 0U; k < 4U; k++)           /* пачка з 4 подій, потім 4 проходи storage */
    {
      post_ev(SL_EV_ENV_WARNING, n++);
    }
    t += 10U;
    fake_platform.now_ms = t;
    (void)SmartLock_FsmStep(t);
    for (unsigned k = 0U; k < 4U; k++)
    {
      TEST_ASSERT_TRUE(poll_captured() != NULL);
      TEST_ASSERT_TRUE(poll_result);
      TEST_ASSERT_EQUAL_UINT(round * 4U + k + 1U, mock_calls);   /* рівно один append за прохід */
    }
  }
  TEST_ASSERT_EQUAL_UINT(100U, n);
  TEST_ASSERT_EQUAL_UINT32(100U, SmartLock_JournalStored());
  TEST_ASSERT_EQUAL_UINT32(0U, SmartLock_JournalUnsaved());
  TEST_ASSERT_EQUAL_UINT32(0U, SmartLock_JournalDropped());
  TEST_ASSERT_EQUAL_UINT(0U, SmartLock_JournalPending());
  unpack_all(recs, 100U);
  for (unsigned i = 0U; i < 100U; i++)
  {
    TEST_ASSERT_EQUAL_UINT32(i + 1U, recs[i].seq);
    TEST_ASSERT_EQUAL_UINT32(i, recs[i].arg);
    TEST_ASSERT_EQUAL_UINT8(SL_LOG_ENV_WARNING, recs[i].type);
    if (i > 0U) { TEST_ASSERT_TRUE(recs[i].t_ms >= recs[i - 1U].t_ms); }
  }

  /* перезапуск МК: RAM скинуто, storage.c після монтування повідомляє останній seq */
  SmartLock_JournalInit();
  SmartLock_FsmInit();
  SmartLock_JournalSetLastSeq(recs[99].seq);
  post_ev(SL_EV_ENV_WARNING, 100U);
  (void)SmartLock_FsmStep(5000U);
  (void)poll_captured();
  unpack_all(recs, 101U);
  TEST_ASSERT_EQUAL_UINT32(101U, recs[100].seq);   /* без дублів і без повернення до 1 */
}

/* Усі типи подій PRD — у правильному порядку, зі зростаючими ID і часом */
static void test_all_prd_event_types_reach_backend_in_order(void)
{
  static const uint8_t expect[] =
  {
    SL_LOG_DOOR_CLOSED, SL_LOG_LOCK_ACTIVATED, SL_LOG_LOCKED, SL_LOG_MOTION, SL_LOG_TAMPER,
    SL_LOG_PHOTO, SL_LOG_PHOTO_ERR, SL_LOG_WRONG_CODE, SL_LOG_FORCED_OPEN, SL_LOG_DOOR_CLOSED,
    SL_LOG_ACCESS_GRANTED, SL_LOG_UNLOCKED, SL_LOG_DOOR_OPEN, SL_LOG_ENV_WARNING,
  };
  sl_log_rec_t recs[16];
  uint32_t ids[2];

  SmartLock_CameraStubSetAccept(true);      /* заглушка приймає обидва запити, результат дамо самі */
  post_ev(SL_EV_DOOR_CLOSED, 0U);
  post_ev(SL_EV_LOCK_REQUEST, SL_LOCK_REQ_T_NOW);
  fake_platform.now_ms = 100U;
  (void)SmartLock_FsmStep(100U);
  fake_platform.now_ms = 100U + SL_EXIT_DELAY_MS;
  (void)SmartLock_FsmStep(100U + SL_EXIT_DELAY_MS);
  TEST_ASSERT_EQUAL(SL_STATE_LOCKED, SmartLock_FsmState());

  fake_platform.now_ms = 20000U;
  post_ev(SL_EV_MOTION, 0U);
  post_ev(SL_EV_TAMPER, 0U);
  (void)SmartLock_FsmStep(20000U);
  {
    sl_log_rec_t r;
    TEST_ASSERT_TRUE(SmartLock_JournalPeek(3U, &r)); ids[0] = r.seq;
    TEST_ASSERT_TRUE(SmartLock_JournalPeek(4U, &r)); ids[1] = r.seq;
  }
  SmartLock_OnCameraResult(ids[0], 11U, SL_CAM_OK);              /* PHOTO */
  SmartLock_OnCameraResult(ids[1], 0U, SL_CAM_ERR_STORE);        /* PHOTO_ERR */
  fake_platform.now_ms = 21000U;
  post_ev(SL_EV_CODE_WRONG, 4U);
  post_ev(SL_EV_DOOR_OPEN, 0U);                                  /* FORCED_OPEN */
  post_ev(SL_EV_DOOR_CLOSED, 0U);
  post_ev(SL_EV_CODE_OK, 4U);                                    /* ACCESS_GRANTED, UNLOCKED */
  post_ev(SL_EV_DOOR_OPEN, 0U);
  post_ev(SL_EV_ENV_WARNING, 2U);
  (void)SmartLock_FsmStep(21000U);
  for (unsigned i = 0U; i < 16U; i++) { (void)poll_captured(); }

  unpack_all(recs, (unsigned)sizeof(expect));
  for (unsigned i = 0U; i < (unsigned)sizeof(expect); i++)
  {
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(expect[i], recs[i].type, SmartLock_LogTypeName((sl_log_type_t)expect[i]));
    TEST_ASSERT_EQUAL_UINT32(i + 1U, recs[i].seq);
    if (i > 0U) { TEST_ASSERT_TRUE(recs[i].t_ms >= recs[i - 1U].t_ms); }
  }
  TEST_ASSERT_EQUAL_UINT32(ids[0], recs[5].ref);
  TEST_ASSERT_EQUAL_UINT32(11U, recs[5].arg);
  TEST_ASSERT_EQUAL_UINT32(ids[1], recs[6].ref);
  TEST_ASSERT_EQUAL_UINT32(SL_CAM_ERR_STORE, recs[6].arg);
  TEST_ASSERT_EQUAL_UINT32(0U, SmartLock_JournalDropped());
}

/* ---- слабка заглушка камери і зв'язок з журналом ----------------------------------------- */
static unsigned count_refs(uint32_t event_id)
{
  sl_log_rec_t r;
  unsigned n = 0U;

  for (unsigned i = 0U; SmartLock_JournalPeek(i, &r); i++)
  {
    if (((r.type == SL_LOG_PHOTO) || (r.type == SL_LOG_PHOTO_ERR)) && (r.ref == event_id)) { n++; }
  }
  return n;
}

static void test_camera_stub_is_controllable_and_rejects_by_default(void)
{
  TEST_ASSERT_FALSE(SmartLock_CameraRequest(5U));
  TEST_ASSERT_EQUAL_UINT(1U, SmartLock_CameraStubCalls());
  TEST_ASSERT_EQUAL_UINT32(5U, SmartLock_CameraStubLastId());
  SmartLock_CameraStubSetAccept(true);
  TEST_ASSERT_TRUE(SmartLock_CameraRequest(6U));
  TEST_ASSERT_EQUAL_UINT32(6U, SmartLock_CameraStubLastId());
  SmartLock_CameraStubReset();
  TEST_ASSERT_EQUAL_UINT(0U, SmartLock_CameraStubCalls());
  TEST_ASSERT_FALSE(SmartLock_CameraRequest(7U));
}

static void test_link_rejected_request_writes_photo_err_immediately(void)
{
  sl_log_rec_t r;
  uint32_t id = SmartLock_LogEvent(SL_LOG_MOTION, 0U);

  TEST_ASSERT_FALSE(SmartLock_CameraLinkStart(id, 0U));   /* заглушка за замовчуванням не приймає */
  TEST_ASSERT_EQUAL_UINT(0U, SmartLock_CameraLinkPending());
  TEST_ASSERT_TRUE(SmartLock_JournalPeek(1U, &r));
  TEST_ASSERT_EQUAL_UINT8(SL_LOG_PHOTO_ERR, r.type);
  TEST_ASSERT_EQUAL_UINT32(SL_CAM_ERR_REJECTED, r.arg);
  TEST_ASSERT_EQUAL_UINT32(id, r.ref);
  TEST_ASSERT_EQUAL_UINT32(1U, SmartLock_CameraLinkStats()->rejected);
  TEST_ASSERT_EQUAL_UINT(1U, count_refs(id));
}

static void test_link_result_ok_and_error_statuses(void)
{
  sl_log_rec_t r;
  uint32_t a;
  uint32_t b;
  uint32_t c;

  SmartLock_CameraStubSetAccept(true);
  a = SmartLock_LogEvent(SL_LOG_MOTION, 0U);
  b = SmartLock_LogEvent(SL_LOG_TAMPER, 0U);
  c = SmartLock_LogEvent(SL_LOG_MOTION, 0U);
  TEST_ASSERT_TRUE(SmartLock_CameraLinkStart(a, 0U));
  TEST_ASSERT_TRUE(SmartLock_CameraLinkStart(b, 0U));
  TEST_ASSERT_TRUE(SmartLock_CameraLinkStart(c, 0U));
  TEST_ASSERT_EQUAL_UINT(3U, SmartLock_CameraLinkPending());

  SmartLock_OnCameraResult(b, 0U, SL_CAM_ERR_CAPTURE);
  SmartLock_OnCameraResult(a, 42U, SL_CAM_OK);
  SmartLock_OnCameraResult(c, 0U, (sl_cam_status_t)99);   /* невідомий статус → CAPTURE */
  TEST_ASSERT_EQUAL_UINT(0U, SmartLock_CameraLinkPending());
  TEST_ASSERT_TRUE(SmartLock_JournalPeek(3U, &r));        /* порядок — як надійшли результати */
  TEST_ASSERT_EQUAL_UINT8(SL_LOG_PHOTO_ERR, r.type);
  TEST_ASSERT_EQUAL_UINT32(SL_CAM_ERR_CAPTURE, r.arg);
  TEST_ASSERT_EQUAL_UINT32(b, r.ref);
  TEST_ASSERT_TRUE(SmartLock_JournalPeek(4U, &r));
  TEST_ASSERT_EQUAL_UINT8(SL_LOG_PHOTO, r.type);
  TEST_ASSERT_EQUAL_UINT32(42U, r.arg);
  TEST_ASSERT_EQUAL_UINT32(a, r.ref);
  TEST_ASSERT_TRUE(SmartLock_JournalPeek(5U, &r));
  TEST_ASSERT_EQUAL_UINT32(SL_CAM_ERR_CAPTURE, r.arg);
  TEST_ASSERT_EQUAL_UINT32(c, r.ref);
  TEST_ASSERT_EQUAL_UINT32(1U, SmartLock_CameraLinkStats()->ok);
  TEST_ASSERT_EQUAL_UINT32(2U, SmartLock_CameraLinkStats()->failed);
  TEST_ASSERT_EQUAL_UINT32(3U, SmartLock_CameraLinkStats()->requested);
}

static void test_link_unknown_duplicate_and_zero_results_are_ignored(void)
{
  uint32_t id;

  SmartLock_CameraStubSetAccept(true);
  id = SmartLock_LogEvent(SL_LOG_MOTION, 0U);
  (void)SmartLock_CameraLinkStart(id, 0U);
  SmartLock_OnCameraResult(id + 50U, 1U, SL_CAM_OK);   /* чужий ID */
  SmartLock_OnCameraResult(SL_LOG_ID_NONE, 1U, SL_CAM_OK);
  TEST_ASSERT_EQUAL_UINT(1U, SmartLock_JournalPending());   /* лише MOTION */
  SmartLock_OnCameraResult(id, 1U, SL_CAM_OK);
  SmartLock_OnCameraResult(id, 2U, SL_CAM_OK);          /* дубль */
  TEST_ASSERT_EQUAL_UINT(1U, count_refs(id));
  TEST_ASSERT_EQUAL_UINT32(3U, SmartLock_CameraLinkStats()->unknown);
}

static void test_link_without_event_id_requests_no_frame(void)
{
  SmartLock_CameraStubSetAccept(true);
  TEST_ASSERT_FALSE(SmartLock_CameraLinkStart(SL_LOG_ID_NONE, 0U));
  TEST_ASSERT_EQUAL_UINT(0U, SmartLock_CameraStubCalls());
  TEST_ASSERT_EQUAL_UINT32(1U, SmartLock_CameraLinkStats()->skipped_no_id);
  TEST_ASSERT_EQUAL_UINT(0U, SmartLock_JournalPending());
}

static void test_link_pending_list_full_gives_rejected_without_calling_camera(void)
{
  uint32_t last = 0U;

  SmartLock_CameraStubSetAccept(true);
  for (unsigned i = 0U; i < TBD_CAMLINK_PENDING_MAX; i++)
  {
    last = SmartLock_LogEvent(SL_LOG_MOTION, 0U);
    TEST_ASSERT_TRUE(SmartLock_CameraLinkStart(last, 0U));
  }
  last = SmartLock_LogEvent(SL_LOG_MOTION, 0U);
  TEST_ASSERT_FALSE(SmartLock_CameraLinkStart(last, 0U));
  TEST_ASSERT_EQUAL_UINT(TBD_CAMLINK_PENDING_MAX, SmartLock_CameraStubCalls());   /* камеру не турбували */
  TEST_ASSERT_EQUAL_UINT(1U, count_refs(last));
  TEST_ASSERT_EQUAL_UINT(TBD_CAMLINK_PENDING_MAX, SmartLock_CameraLinkPending());
}

static void test_link_timeout_gives_single_photo_err_and_late_result_is_ignored(void)
{
  sl_log_rec_t r;
  uint32_t id;
  const uint32_t t0 = 0xFFFFFFFFU - 3000U;   /* відлік через переповнення мілісекунд */

  SmartLock_CameraStubSetAccept(true);
  id = SmartLock_LogEvent(SL_LOG_TAMPER, 0U);
  TEST_ASSERT_TRUE(SmartLock_CameraLinkStart(id, t0));
  TEST_ASSERT_FALSE(SmartLock_CameraLinkPoll(t0 + TBD_CAM_RESULT_TIMEOUT_MS - 1U));
  TEST_ASSERT_EQUAL_UINT(0U, count_refs(id));
  TEST_ASSERT_TRUE(SmartLock_CameraLinkPoll(t0 + TBD_CAM_RESULT_TIMEOUT_MS));
  TEST_ASSERT_EQUAL_UINT(1U, count_refs(id));
  TEST_ASSERT_TRUE(SmartLock_JournalPeek(1U, &r));
  TEST_ASSERT_EQUAL_UINT8(SL_LOG_PHOTO_ERR, r.type);
  TEST_ASSERT_EQUAL_UINT32(SL_CAM_ERR_TIMEOUT, r.arg);
  TEST_ASSERT_EQUAL_UINT(0U, SmartLock_CameraLinkPending());
  TEST_ASSERT_FALSE(SmartLock_CameraLinkPoll(t0 + TBD_CAM_RESULT_TIMEOUT_MS + 1U));

  SmartLock_OnCameraResult(id, 9U, SL_CAM_OK);          /* камера все ж відповіла — запису вже немає чекати */
  TEST_ASSERT_EQUAL_UINT(1U, count_refs(id));
  TEST_ASSERT_EQUAL_UINT32(1U, SmartLock_CameraLinkStats()->timeouts);
  TEST_ASSERT_EQUAL_UINT32(1U, SmartLock_CameraLinkStats()->unknown);
}

/* Журнал не прийняв PHOTO (FIFO повна навіть із резервом): результат чекає, запис не губиться */
static void test_link_retries_journal_write_until_space(void)
{
  uint32_t id;

  SmartLock_CameraStubSetAccept(true);
  id = SmartLock_LogEvent(SL_LOG_MOTION, 0U);
  TEST_ASSERT_TRUE(SmartLock_CameraLinkStart(id, 0U));
  while (SmartLock_JournalPending() < TBD_JOURNAL_FIFO_LEN)
  {
    (void)SmartLock_LogEventRef(SL_LOG_PHOTO, 1U, 999U);   /* займаємо і резерв */
  }
  SmartLock_OnCameraResult(id, 5U, SL_CAM_OK);
  TEST_ASSERT_EQUAL_UINT(1U, SmartLock_CameraLinkPending());   /* результат є, запису ще немає */
  TEST_ASSERT_EQUAL_UINT(0U, count_refs(id));
  TEST_ASSERT_FALSE(SmartLock_CameraLinkPoll(10U));            /* місця все ще немає */

  (void)poll_captured();                                       /* storage звільнила місце */
  TEST_ASSERT_TRUE(SmartLock_CameraLinkPoll(20U));
  TEST_ASSERT_EQUAL_UINT(0U, SmartLock_CameraLinkPending());
  TEST_ASSERT_EQUAL_UINT(1U, count_refs(id));
}

static void test_status_names(void)
{
  TEST_ASSERT_EQUAL_STRING("OK", SmartLock_CameraStatusName(SL_CAM_OK));
  TEST_ASSERT_EQUAL_STRING("REJECTED", SmartLock_CameraStatusName(SL_CAM_ERR_REJECTED));
  TEST_ASSERT_EQUAL_STRING("CAPTURE", SmartLock_CameraStatusName(SL_CAM_ERR_CAPTURE));
  TEST_ASSERT_EQUAL_STRING("STORE", SmartLock_CameraStatusName(SL_CAM_ERR_STORE));
  TEST_ASSERT_EQUAL_STRING("TIMEOUT", SmartLock_CameraStatusName(SL_CAM_ERR_TIMEOUT));
  TEST_ASSERT_EQUAL_STRING("?", SmartLock_CameraStatusName(SL_CAM_STATUS_COUNT));
}

int main(void)
{
  UNITY_BEGIN();
  RUN_TEST(test_log_assigns_increasing_ids_and_time);
  RUN_TEST(test_invalid_type_is_rejected_without_consuming_id);
  RUN_TEST(test_overflow_drops_new_counts_and_leaves_id_gap);
  RUN_TEST(test_storage_poll_one_record_per_pass_in_order);
  RUN_TEST(test_backend_failures_count_unsaved_and_do_not_block);
  RUN_TEST(test_dead_flash_keeps_uart_journal_running);
  RUN_TEST(test_fifo_wraps_around);
  RUN_TEST(test_reserve_slots_are_only_for_photo_records);
  RUN_TEST(test_restored_sequence_and_wrap_skip_zero);
  RUN_TEST(test_set_last_seq_continues_numbering_mid_run);
  RUN_TEST(test_hundred_events_through_pipeline_without_loss);
  RUN_TEST(test_all_prd_event_types_reach_backend_in_order);
  RUN_TEST(test_camera_stub_is_controllable_and_rejects_by_default);
  RUN_TEST(test_link_rejected_request_writes_photo_err_immediately);
  RUN_TEST(test_link_result_ok_and_error_statuses);
  RUN_TEST(test_link_unknown_duplicate_and_zero_results_are_ignored);
  RUN_TEST(test_link_without_event_id_requests_no_frame);
  RUN_TEST(test_link_pending_list_full_gives_rejected_without_calling_camera);
  RUN_TEST(test_link_timeout_gives_single_photo_err_and_late_result_is_ignored);
  RUN_TEST(test_link_retries_journal_write_until_space);
  RUN_TEST(test_status_names);
  RUN_TEST(test_type_names_match_prd);
  return UNITY_END();
}
