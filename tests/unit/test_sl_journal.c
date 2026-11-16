/**
  * @file    test_sl_journal.c
  * @brief   ЛР3 (K07/K09): журнал подій у RAM (sl_journal.c) — порядок і ID,
  *          переповнення FIFO, задача storage (одна порція за прохід, рядок EVT
  *          у UART) і бекенд через sl_journal_ops_t.
  *
  *          Бекенд — мок: тест визначає сильну SmartLock_JournalOps(), як це
  *          зробить журнал W25Q64 (H09). Слабкий бекенд «Flash недоступний»
  *          перевіряє test_sl_fsm.c (там сильної версії немає).
  */
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "unity.h"

#include "fake_platform.h"
#include "platform.h"
#include "sl_journal.h"
#include "smartlock_hooks.h"

/* ---- мок бекенду ------------------------------------------------------------------ */
static dev_status_t mock_status;
static bool         mock_no_ops;        /* SmartLock_JournalOps() → NULL */
static bool         mock_no_append;     /* ops без append */
static unsigned     mock_calls;
static sl_log_rec_t mock_last;

static dev_status_t mock_append(const sl_log_rec_t *rec)
{
  mock_calls++;
  mock_last = *rec;
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
  SmartLock_JournalInit();
  mock_status = DEV_OK;
  mock_no_ops = false;
  mock_no_append = false;
  mock_calls = 0U;
  memset(&mock_last, 0, sizeof(mock_last));
}

void tearDown(void)
{
}

/* ============================================================================ */

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

  for (unsigned i = 0U; i < TBD_JOURNAL_FIFO_LEN; i++)
  {
    TEST_ASSERT_EQUAL_UINT32(i + 1U, SmartLock_LogEvent(SL_LOG_MOTION, i));
  }
  TEST_ASSERT_EQUAL_UINT32(SL_LOG_ID_NONE, SmartLock_LogEvent(SL_LOG_TAMPER, 99U));   /* FIFO повна */
  TEST_ASSERT_EQUAL_UINT32(1U, SmartLock_JournalDropped());
  TEST_ASSERT_EQUAL_UINT(TBD_JOURNAL_FIFO_LEN, SmartLock_JournalPending());
  TEST_ASSERT_EQUAL_UINT(TBD_JOURNAL_FIFO_LEN, SmartLock_JournalMaxDepth());

  /* найстаріші записи не перезаписано */
  TEST_ASSERT_TRUE(SmartLock_JournalPeek(0U, &r));
  TEST_ASSERT_EQUAL_UINT32(1U, r.seq);

  /* звільнили місце: новий запис отримує ID після втраченого — пропуск видно */
  (void)poll_captured();
  TEST_ASSERT_EQUAL_UINT32(TBD_JOURNAL_FIFO_LEN + 2U, SmartLock_LogEvent(SL_LOG_TAMPER, 1U));
  TEST_ASSERT_TRUE(SmartLock_JournalPeek(TBD_JOURNAL_FIFO_LEN - 1U, &r));
  TEST_ASSERT_EQUAL_UINT32(TBD_JOURNAL_FIFO_LEN + 2U, r.seq);
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

  mock_status = DEV_ERR_TIMEOUT;   /* Flash зайнята / помилка запису */
  (void)SmartLock_LogEvent(SL_LOG_FORCED_OPEN, 0U);
  o = poll_captured();
  TEST_ASSERT_EQUAL_STRING("EVT,id=1,t_ms=0,name=FORCED_OPEN,arg=0,flash=err\r\n", o);
  TEST_ASSERT_EQUAL_UINT32(1U, SmartLock_JournalUnsaved());
  TEST_ASSERT_EQUAL_UINT(0U, SmartLock_JournalPending());   /* запис не застряг у FIFO */

  mock_no_append = true;
  (void)SmartLock_LogEvent(SL_LOG_ENV_WARNING, 3U);
  (void)poll_captured();
  mock_no_ops = true;
  (void)SmartLock_LogEvent(SL_LOG_DOOR_OPEN, 0U);
  (void)poll_captured();
  TEST_ASSERT_EQUAL_UINT32(3U, SmartLock_JournalUnsaved());
  TEST_ASSERT_EQUAL_UINT32(0U, SmartLock_JournalStored());
  TEST_ASSERT_EQUAL_UINT(1U, mock_calls);
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
    "MOTION", "TAMPER", "FORCED_OPEN", "ENV_WARNING", "DOOR_OPEN", "DOOR_CLOSED", "PHOTO",
  };

  for (unsigned t = 0U; t < (unsigned)SL_LOG_COUNT; t++)
  {
    TEST_ASSERT_EQUAL_STRING(names[t], SmartLock_LogTypeName((sl_log_type_t)t));
  }
  TEST_ASSERT_EQUAL_STRING("?", SmartLock_LogTypeName(SL_LOG_COUNT));
}

int main(void)
{
  UNITY_BEGIN();
  RUN_TEST(test_log_assigns_increasing_ids_and_time);
  RUN_TEST(test_invalid_type_is_rejected_without_consuming_id);
  RUN_TEST(test_overflow_drops_new_counts_and_leaves_id_gap);
  RUN_TEST(test_storage_poll_one_record_per_pass_in_order);
  RUN_TEST(test_backend_failures_count_unsaved_and_do_not_block);
  RUN_TEST(test_fifo_wraps_around);
  RUN_TEST(test_restored_sequence_and_wrap_skip_zero);
  RUN_TEST(test_type_names_match_prd);
  return UNITY_END();
}
