/**
  * @file    test_sl_fsm.c
  * @brief   ЛР3 (K02–K05/K09): FSM SmartLock (sl_fsm.c) без плати.
  *
  *          PRD D.1: UNLOCKED → EXIT_DELAY → LOCKED → UNLOCKED без автоблокування;
  *          15 с на межі (14 999 / 15 000 мс) і через переповнення мілісекунд;
  *          двері відчинені / зачинені після відліку, закриття без другого
  *          відліку; FORCED_OPEN і його відсутність після авторизованого
  *          розблокування; CODE_OK / CODE_WRONG; MOTION / TAMPER лише в LOCKED.
  *
  *          Входи — події в черзі (event_queue.h), як їх кладуть buttons /
  *          security / env. Виходи: серво — fake_platform.servo_log, журнал —
  *          справжній sl_journal.c (SmartLock_JournalPeek), камера — сильна
  *          SmartLock_CameraRequest() цього тесту. Бекенд журналу — слабкий
  *          («Flash недоступний»).
  */
#include <string.h>

#include "unity.h"

#include "app.h"
#include "app_tasks.h"
#include "event_queue.h"
#include "fake_i2c.h"
#include "fake_platform.h"
#include "platform.h"
#include "sched.h"
#include "sl_camera_link.h"
#include "sl_fsm.h"
#include "sl_journal.h"
#include "smartlock_hooks.h"

/* ---- камера: перехоплення запитів (сильна версія замість заглушки) ---------------- */
#define CAM_LOG_LEN 8U
static uint32_t cam_ids[CAM_LOG_LEN];
static unsigned cam_calls;
static bool     cam_accept;

bool SmartLock_CameraRequest(uint32_t event_id)
{
  if (cam_calls < CAM_LOG_LEN)
  {
    cam_ids[cam_calls] = event_id;
  }
  cam_calls++;
  return cam_accept;
}

/* ---- помічники ---------------------------------------------------------------------- */
static void post(sl_event_type_t type, uint32_t arg)
{
  TEST_ASSERT_TRUE(SmartLock_PostEvent(type, arg));
}

/* Один прохід FSM у момент now (той самий час бачить і журнал) */
static bool step(uint32_t now)
{
  fake_platform.now_ms = now;
  return SmartLock_FsmStep(now);
}

static void expect_log(unsigned i, sl_log_type_t type, uint32_t arg)
{
  sl_log_rec_t r;

  TEST_ASSERT_TRUE_MESSAGE(SmartLock_JournalPeek(i, &r), "немає запису журналу");
  TEST_ASSERT_EQUAL_STRING(SmartLock_LogTypeName(type), SmartLock_LogTypeName((sl_log_type_t)r.type));
  TEST_ASSERT_EQUAL_UINT32(arg, r.arg);
}

static uint32_t log_seq(unsigned i)
{
  sl_log_rec_t r;

  TEST_ASSERT_TRUE(SmartLock_JournalPeek(i, &r));
  return r.seq;
}

static uint32_t servo_last(void)
{
  return fake_platform.servo_pulse_us;
}

/* Від t0: двері зачинено, B1, 15 с → LOCKED. Журнал після цього очищено. */
static void go_locked(uint32_t t0)
{
  post(SL_EV_DOOR_CLOSED, 0U);
  post(SL_EV_LOCK_REQUEST, SL_LOCK_REQ_T_NOW);
  (void)step(t0);
  (void)step(t0 + SL_EXIT_DELAY_MS);
  TEST_ASSERT_EQUAL(SL_STATE_LOCKED, SmartLock_FsmState());
  SmartLock_JournalInit();
  cam_calls = 0U;
}

void setUp(void)
{
  fake_platform_reset();
  fake_i2c_reset();
  memset(&app_devices, 0, sizeof(app_devices));
  app_devices.mpu_status  = DEV_ERR_STATE;
  app_devices.baro_status = DEV_ERR_STATE;
  SmartLock_EventsInit();
  SmartLock_JournalInit();
  SmartLock_FsmInit();
  memset(cam_ids, 0, sizeof(cam_ids));
  cam_calls = 0U;
  cam_accept = true;
}

void tearDown(void)
{
}

/* ============================================================================ */

static void test_init_is_unlocked_with_servo_unlock(void)
{
  TEST_ASSERT_EQUAL(SL_STATE_UNLOCKED, SmartLock_FsmState());
  TEST_ASSERT_EQUAL(SL_DOOR_UNKNOWN, SmartLock_FsmDoor());
  TEST_ASSERT_EQUAL_UINT32(TBD_SERVO_UNLOCK_US, servo_last());
  TEST_ASSERT_EQUAL_UINT32(1U, fake_platform.servo_set_calls);
  TEST_ASSERT_EQUAL_UINT(0U, SmartLock_JournalPending());
  TEST_ASSERT_FALSE(step(0U));   /* без подій і без відліку — немає роботи */
  TEST_ASSERT_EQUAL_STRING("UNLOCKED", SmartLock_FsmStateName(SL_STATE_UNLOCKED));
  TEST_ASSERT_EQUAL_STRING("EXIT_DELAY", SmartLock_FsmStateName(SL_STATE_EXIT_DELAY));
  TEST_ASSERT_EQUAL_STRING("LOCKED", SmartLock_FsmStateName(SL_STATE_LOCKED));
  TEST_ASSERT_EQUAL_STRING("?", SmartLock_FsmStateName(SL_STATE_COUNT));
}

static void test_lock_request_starts_exit_delay(void)
{
  post(SL_EV_LOCK_REQUEST, SL_LOCK_REQ_T_NOW);
  TEST_ASSERT_TRUE(step(1000U));
  TEST_ASSERT_EQUAL(SL_STATE_EXIT_DELAY, SmartLock_FsmState());
  TEST_ASSERT_EQUAL_UINT32(SL_EXIT_DELAY_MS, SmartLock_FsmExitDelayRemainingMs(1000U));
  TEST_ASSERT_EQUAL_UINT32(5000U, SmartLock_FsmExitDelayRemainingMs(11000U));
  TEST_ASSERT_FALSE(SmartLock_FsmWaitingDoor());
  TEST_ASSERT_EQUAL_UINT(1U, SmartLock_JournalPending());
  expect_log(0U, SL_LOG_LOCK_ACTIVATED, SL_EXIT_DELAY_MS);
  TEST_ASSERT_EQUAL_UINT32(TBD_SERVO_UNLOCK_US, servo_last());   /* FR-03: до кінця відліку UNLOCK */
  TEST_ASSERT_EQUAL_UINT32(1U, fake_platform.servo_set_calls);
}

static void test_door_closed_locks_exactly_at_15000_ms(void)
{
  post(SL_EV_DOOR_CLOSED, 0U);
  post(SL_EV_LOCK_REQUEST, SL_LOCK_REQ_T_NOW);
  (void)step(500U);

  TEST_ASSERT_FALSE(step(500U + 14999U));
  TEST_ASSERT_EQUAL(SL_STATE_EXIT_DELAY, SmartLock_FsmState());
  TEST_ASSERT_EQUAL_UINT32(1U, SmartLock_FsmExitDelayRemainingMs(500U + 14999U));
  TEST_ASSERT_EQUAL_UINT32(TBD_SERVO_UNLOCK_US, servo_last());

  TEST_ASSERT_TRUE(step(500U + 15000U));
  TEST_ASSERT_EQUAL(SL_STATE_LOCKED, SmartLock_FsmState());
  TEST_ASSERT_EQUAL_UINT32(2U, fake_platform.servo_set_calls);
  TEST_ASSERT_EQUAL_UINT32(TBD_SERVO_LOCK_US, fake_platform.servo_log[1]);
  TEST_ASSERT_EQUAL_UINT32(15500U, fake_platform.servo_log_ms[1]);   /* 15 000 мс від прийнятого B1 */
  TEST_ASSERT_EQUAL_UINT32(0U, SmartLock_FsmExitDelayRemainingMs(15500U));
  /* DOOR_CLOSED (вимикач TBD_SL_LOG_DOOR_EVENTS = 1), LOCK_ACTIVATED, LOCKED */
  expect_log(0U, SL_LOG_DOOR_CLOSED, 0U);
  expect_log(1U, SL_LOG_LOCK_ACTIVATED, SL_EXIT_DELAY_MS);
  expect_log(2U, SL_LOG_LOCKED, SL_EXIT_DELAY_MS);
  TEST_ASSERT_EQUAL_UINT(3U, SmartLock_JournalPending());
}

static void test_exit_delay_survives_millis_overflow(void)
{
  const uint32_t t0 = 0xFFFFFFFFU - 5000U;   /* відлік проходить через 0 */

  post(SL_EV_DOOR_CLOSED, 0U);
  post(SL_EV_LOCK_REQUEST, SL_LOCK_REQ_T_NOW);
  (void)step(t0);
  TEST_ASSERT_FALSE(step(t0 + 14999U));      /* = 9 998 після переповнення */
  TEST_ASSERT_EQUAL(SL_STATE_EXIT_DELAY, SmartLock_FsmState());
  TEST_ASSERT_EQUAL_UINT32(1U, SmartLock_FsmExitDelayRemainingMs(t0 + 14999U));
  TEST_ASSERT_TRUE(step(t0 + 15000U));
  TEST_ASSERT_EQUAL(SL_STATE_LOCKED, SmartLock_FsmState());
  expect_log(2U, SL_LOG_LOCKED, SL_EXIT_DELAY_MS);
}

static void test_countdown_starts_from_accepted_press_stamp(void)
{
  /* buttons прийняла B1 у 1000 мс, FSM забрала подію в 1030 мс */
  post(SL_EV_DOOR_CLOSED, 0U);
  post(SL_EV_LOCK_REQUEST, 1000U);
  (void)step(1030U);
  TEST_ASSERT_EQUAL_UINT32(SL_EXIT_DELAY_MS - 30U, SmartLock_FsmExitDelayRemainingMs(1030U));
  TEST_ASSERT_FALSE(step(15999U));
  TEST_ASSERT_TRUE(step(16000U));
  TEST_ASSERT_EQUAL(SL_STATE_LOCKED, SmartLock_FsmState());
}

static void test_invalid_press_stamp_falls_back_to_now(void)
{
  /* мітка «з майбутнього» */
  post(SL_EV_LOCK_REQUEST, 5000U);
  (void)step(2000U);
  TEST_ASSERT_EQUAL_UINT32(SL_EXIT_DELAY_MS, SmartLock_FsmExitDelayRemainingMs(2000U));

  /* мітка старша за весь відлік — не блокувати миттєво */
  SmartLock_FsmInit();
  post(SL_EV_LOCK_REQUEST, 1U);
  (void)step(100000U);
  TEST_ASSERT_EQUAL_UINT32(SL_EXIT_DELAY_MS, SmartLock_FsmExitDelayRemainingMs(100000U));
}

static void test_door_open_after_delay_waits_then_locks_without_second_countdown(void)
{
  post(SL_EV_DOOR_OPEN, 0U);
  post(SL_EV_LOCK_REQUEST, SL_LOCK_REQ_T_NOW);
  (void)step(0U);

  TEST_ASSERT_TRUE(step(15000U));   /* відлік завершено — внутрішня умова EXIT_DELAY змінилась */
  TEST_ASSERT_EQUAL(SL_STATE_EXIT_DELAY, SmartLock_FsmState());
  TEST_ASSERT_TRUE(SmartLock_FsmWaitingDoor());
  TEST_ASSERT_EQUAL_UINT32(0U, SmartLock_FsmExitDelayRemainingMs(15000U));
  TEST_ASSERT_EQUAL_UINT32(TBD_SERVO_UNLOCK_US, servo_last());
  TEST_ASSERT_EQUAL_UINT32(1U, fake_platform.servo_set_calls);   /* лише UNLOCK з init — LOCK немає */

  /* хвилину двері відчинені: нічого не відбувається */
  for (uint32_t t = 15001U; t <= 75000U; t += 1000U)
  {
    TEST_ASSERT_FALSE(step(t));
  }
  TEST_ASSERT_EQUAL_UINT32(1U, fake_platform.servo_set_calls);

  /* зачинили — LOCK у цьому ж проході, без нових 15 с */
  post(SL_EV_DOOR_CLOSED, 0U);
  TEST_ASSERT_TRUE(step(80000U));
  TEST_ASSERT_EQUAL(SL_STATE_LOCKED, SmartLock_FsmState());
  TEST_ASSERT_FALSE(SmartLock_FsmWaitingDoor());
  TEST_ASSERT_EQUAL_UINT32(TBD_SERVO_LOCK_US, servo_last());
  TEST_ASSERT_EQUAL_UINT32(80000U, fake_platform.servo_log_ms[1]);
  /* DOOR_OPEN, LOCK_ACTIVATED, DOOR_CLOSED, LOCKED (arg — фактична затримка) */
  expect_log(0U, SL_LOG_DOOR_OPEN, 0U);
  expect_log(1U, SL_LOG_LOCK_ACTIVATED, SL_EXIT_DELAY_MS);
  expect_log(2U, SL_LOG_DOOR_CLOSED, 0U);
  expect_log(3U, SL_LOG_LOCKED, 80000U);
}

static void test_unknown_door_is_treated_as_open(void)
{
  post(SL_EV_LOCK_REQUEST, SL_LOCK_REQ_T_NOW);
  (void)step(0U);
  (void)step(20000U);
  TEST_ASSERT_EQUAL(SL_STATE_EXIT_DELAY, SmartLock_FsmState());
  TEST_ASSERT_TRUE(SmartLock_FsmWaitingDoor());
  TEST_ASSERT_EQUAL_UINT32(1U, fake_platform.servo_set_calls);
  post(SL_EV_DOOR_CLOSED, 0U);
  (void)step(20010U);
  TEST_ASSERT_EQUAL(SL_STATE_LOCKED, SmartLock_FsmState());
}

static void test_door_opened_and_closed_during_countdown_still_locks_on_time(void)
{
  post(SL_EV_DOOR_CLOSED, 0U);
  post(SL_EV_LOCK_REQUEST, SL_LOCK_REQ_T_NOW);
  (void)step(0U);
  post(SL_EV_DOOR_OPEN, 0U);
  (void)step(3000U);
  post(SL_EV_DOOR_CLOSED, 0U);
  (void)step(9000U);
  TEST_ASSERT_EQUAL_UINT32(6000U, SmartLock_FsmExitDelayRemainingMs(9000U));   /* відлік не перезапущено */
  TEST_ASSERT_TRUE(step(15000U));
  TEST_ASSERT_EQUAL(SL_STATE_LOCKED, SmartLock_FsmState());
}

static void test_door_open_in_same_pass_as_deadline_prevents_lock(void)
{
  post(SL_EV_DOOR_CLOSED, 0U);
  post(SL_EV_LOCK_REQUEST, SL_LOCK_REQ_T_NOW);
  (void)step(0U);
  post(SL_EV_DOOR_OPEN, 0U);   /* security побачила відкриття перед самим кінцем відліку */
  (void)step(15000U);
  TEST_ASSERT_EQUAL(SL_STATE_EXIT_DELAY, SmartLock_FsmState());
  TEST_ASSERT_TRUE(SmartLock_FsmWaitingDoor());
  TEST_ASSERT_EQUAL_UINT32(1U, fake_platform.servo_set_calls);
}

static void test_repeated_lock_request_does_not_restart_countdown(void)
{
  post(SL_EV_DOOR_CLOSED, 0U);
  post(SL_EV_LOCK_REQUEST, SL_LOCK_REQ_T_NOW);
  (void)step(0U);
  post(SL_EV_LOCK_REQUEST, SL_LOCK_REQ_T_NOW);   /* E2E-01: швидкі повторні B1 */
  post(SL_EV_LOCK_REQUEST, 9000U);
  (void)step(9000U);
  TEST_ASSERT_EQUAL_UINT32(2U, SmartLock_FsmIgnored());
  TEST_ASSERT_EQUAL_UINT32(6000U, SmartLock_FsmExitDelayRemainingMs(9000U));
  TEST_ASSERT_TRUE(step(15000U));
  TEST_ASSERT_EQUAL(SL_STATE_LOCKED, SmartLock_FsmState());
  /* один LOCK_ACTIVATED: DOOR_CLOSED, LOCK_ACTIVATED, LOCKED */
  TEST_ASSERT_EQUAL_UINT(3U, SmartLock_JournalPending());

  /* B1 у LOCKED — теж без ефекту */
  post(SL_EV_LOCK_REQUEST, SL_LOCK_REQ_T_NOW);
  (void)step(16000U);
  TEST_ASSERT_EQUAL(SL_STATE_LOCKED, SmartLock_FsmState());
  TEST_ASSERT_EQUAL_UINT(3U, SmartLock_JournalPending());
}

static void test_motion_and_tamper_in_locked_log_and_request_photo(void)
{
  go_locked(0U);
  post(SL_EV_MOTION, 0U);
  TEST_ASSERT_TRUE(step(20000U));
  TEST_ASSERT_EQUAL(SL_STATE_LOCKED, SmartLock_FsmState());
  expect_log(0U, SL_LOG_MOTION, 0U);
  TEST_ASSERT_EQUAL_UINT(1U, cam_calls);
  TEST_ASSERT_EQUAL_UINT32(log_seq(0U), cam_ids[0]);   /* кадр прив'язаний до ID події */

  post(SL_EV_TAMPER, 1234U);
  (void)step(21000U);
  expect_log(1U, SL_LOG_TAMPER, 1234U);
  TEST_ASSERT_EQUAL_UINT(2U, cam_calls);
  TEST_ASSERT_EQUAL_UINT32(log_seq(1U), cam_ids[1]);
  TEST_ASSERT_EQUAL_UINT32(TBD_SERVO_LOCK_US, servo_last());
}

static void test_simultaneous_motion_and_tamper_give_two_records(void)
{
  go_locked(0U);
  post(SL_EV_MOTION, 0U);   /* E2E-02: обидві події в одному проході */
  post(SL_EV_TAMPER, 900U);
  (void)step(20000U);
  TEST_ASSERT_EQUAL(SL_STATE_LOCKED, SmartLock_FsmState());
  TEST_ASSERT_EQUAL_UINT(2U, SmartLock_JournalPending());
  expect_log(0U, SL_LOG_MOTION, 0U);
  expect_log(1U, SL_LOG_TAMPER, 900U);
  TEST_ASSERT_EQUAL_UINT(2U, cam_calls);
  TEST_ASSERT_NOT_EQUAL(cam_ids[0], cam_ids[1]);
  TEST_ASSERT_EQUAL_UINT32(log_seq(0U), cam_ids[0]);
  TEST_ASSERT_EQUAL_UINT32(log_seq(1U), cam_ids[1]);
}

static void test_security_and_code_events_ignored_outside_locked(void)
{
  static const sl_event_type_t evs[] = { SL_EV_MOTION, SL_EV_TAMPER, SL_EV_CODE_OK, SL_EV_CODE_WRONG };

  /* UNLOCKED */
  for (unsigned i = 0U; i < 4U; i++) { post(evs[i], 1U); }
  (void)step(100U);
  TEST_ASSERT_EQUAL(SL_STATE_UNLOCKED, SmartLock_FsmState());
  TEST_ASSERT_EQUAL_UINT(0U, SmartLock_JournalPending());

  /* EXIT_DELAY */
  post(SL_EV_LOCK_REQUEST, SL_LOCK_REQ_T_NOW);
  (void)step(200U);
  for (unsigned i = 0U; i < 4U; i++) { post(evs[i], 1U); }
  (void)step(300U);
  TEST_ASSERT_EQUAL(SL_STATE_EXIT_DELAY, SmartLock_FsmState());
  TEST_ASSERT_EQUAL_UINT(1U, SmartLock_JournalPending());   /* лише LOCK_ACTIVATED */
  TEST_ASSERT_EQUAL_UINT(0U, cam_calls);
  TEST_ASSERT_EQUAL_UINT32(8U, SmartLock_FsmIgnored());
  TEST_ASSERT_EQUAL_UINT32(1U, fake_platform.servo_set_calls);   /* CODE_OK не чіпав серво */
}

static void test_forced_open_keeps_locked_without_photo(void)
{
  go_locked(0U);
  post(SL_EV_DOOR_OPEN, 0U);
  (void)step(30000U);
  TEST_ASSERT_EQUAL(SL_STATE_LOCKED, SmartLock_FsmState());   /* не вважається авторизацією */
  TEST_ASSERT_EQUAL(SL_DOOR_OPEN, SmartLock_FsmDoor());
  TEST_ASSERT_EQUAL_UINT(1U, SmartLock_JournalPending());   /* лише FORCED_OPEN, без DOOR_OPEN */
  expect_log(0U, SL_LOG_FORCED_OPEN, 0U);
  TEST_ASSERT_EQUAL_UINT(0U, cam_calls);
  TEST_ASSERT_EQUAL_UINT32(TBD_SERVO_LOCK_US, servo_last());

  /* повторна DOOR_OPEN без DOOR_CLOSED — не новий FORCED_OPEN */
  post(SL_EV_DOOR_OPEN, 0U);
  (void)step(30100U);
  TEST_ASSERT_EQUAL_UINT(1U, SmartLock_JournalPending());

  /* зачинили і знову відчинили — новий FORCED_OPEN */
  post(SL_EV_DOOR_CLOSED, 0U);
  post(SL_EV_DOOR_OPEN, 0U);
  (void)step(31000U);
  expect_log(1U, SL_LOG_DOOR_CLOSED, 0U);
  expect_log(2U, SL_LOG_FORCED_OPEN, 0U);

  /* правильний код після FORCED_OPEN усе одно розблоковує */
  post(SL_EV_CODE_OK, 4U);
  (void)step(32000U);
  TEST_ASSERT_EQUAL(SL_STATE_UNLOCKED, SmartLock_FsmState());
}

static void test_no_forced_open_after_authorized_unlock(void)
{
  go_locked(0U);
  post(SL_EV_CODE_OK, 4U);
  post(SL_EV_DOOR_OPEN, 0U);   /* користувач відчиняє двері одразу після коду */
  (void)step(30000U);
  TEST_ASSERT_EQUAL(SL_STATE_UNLOCKED, SmartLock_FsmState());
  expect_log(0U, SL_LOG_ACCESS_GRANTED, 4U);
  expect_log(1U, SL_LOG_UNLOCKED, 0U);
  expect_log(2U, SL_LOG_DOOR_OPEN, 0U);
  TEST_ASSERT_EQUAL_UINT(3U, SmartLock_JournalPending());
}

static void test_wrong_then_right_code(void)
{
  uint32_t calls;

  go_locked(0U);
  calls = fake_platform.servo_set_calls;
  post(SL_EV_CODE_WRONG, 4U);
  (void)step(20000U);
  TEST_ASSERT_EQUAL(SL_STATE_LOCKED, SmartLock_FsmState());
  expect_log(0U, SL_LOG_WRONG_CODE, 4U);
  TEST_ASSERT_EQUAL_UINT32(calls, fake_platform.servo_set_calls);   /* засувку не чіпали */
  TEST_ASSERT_EQUAL_UINT32(TBD_SERVO_LOCK_US, servo_last());

  post(SL_EV_CODE_OK, 4U);
  (void)step(25000U);
  TEST_ASSERT_EQUAL(SL_STATE_UNLOCKED, SmartLock_FsmState());
  TEST_ASSERT_EQUAL_UINT32(TBD_SERVO_UNLOCK_US, servo_last());
  TEST_ASSERT_EQUAL_UINT32(calls + 1U, fake_platform.servo_set_calls);
  expect_log(1U, SL_LOG_ACCESS_GRANTED, 4U);
  expect_log(2U, SL_LOG_UNLOCKED, 0U);
}

static void test_no_auto_lock_after_unlock(void)
{
  uint32_t calls;

  go_locked(0U);
  post(SL_EV_CODE_OK, 4U);
  (void)step(20000U);
  calls = fake_platform.servo_set_calls;
  SmartLock_JournalInit();
  /* PRD D.2 FR-11: 5 хв після розблокування, двері зачинені */
  for (uint32_t t = 20000U; t <= (20000U + 300000U); t += 500U)
  {
    TEST_ASSERT_FALSE(step(t));
  }
  TEST_ASSERT_EQUAL(SL_STATE_UNLOCKED, SmartLock_FsmState());
  TEST_ASSERT_EQUAL_UINT32(calls, fake_platform.servo_set_calls);
  TEST_ASSERT_EQUAL_UINT(0U, SmartLock_JournalPending());

  /* і повний цикл знову доступний ручною командою */
  post(SL_EV_LOCK_REQUEST, SL_LOCK_REQ_T_NOW);
  (void)step(400000U);
  TEST_ASSERT_TRUE(step(415000U));
  TEST_ASSERT_EQUAL(SL_STATE_LOCKED, SmartLock_FsmState());
}

static void test_env_warning_logged_in_every_state_without_transition(void)
{
  post(SL_EV_ENV_WARNING, 1U);
  (void)step(0U);
  TEST_ASSERT_EQUAL(SL_STATE_UNLOCKED, SmartLock_FsmState());
  expect_log(0U, SL_LOG_ENV_WARNING, 1U);

  post(SL_EV_DOOR_CLOSED, 0U);
  post(SL_EV_LOCK_REQUEST, SL_LOCK_REQ_T_NOW);
  post(SL_EV_ENV_WARNING, 2U);
  (void)step(1000U);
  TEST_ASSERT_EQUAL(SL_STATE_EXIT_DELAY, SmartLock_FsmState());
  expect_log(3U, SL_LOG_ENV_WARNING, 2U);
  TEST_ASSERT_EQUAL_UINT32(SL_EXIT_DELAY_MS, SmartLock_FsmExitDelayRemainingMs(1000U));

  (void)step(16000U);
  post(SL_EV_ENV_WARNING, 3U);
  (void)step(17000U);
  TEST_ASSERT_EQUAL(SL_STATE_LOCKED, SmartLock_FsmState());
  expect_log(5U, SL_LOG_ENV_WARNING, 3U);
  TEST_ASSERT_EQUAL_UINT32(TBD_SERVO_LOCK_US, servo_last());
  TEST_ASSERT_EQUAL_UINT(0U, cam_calls);
}

static void test_duplicate_door_events_are_ignored(void)
{
  post(SL_EV_DOOR_CLOSED, 0U);
  post(SL_EV_DOOR_CLOSED, 0U);
  (void)step(0U);
  TEST_ASSERT_EQUAL(SL_DOOR_CLOSED, SmartLock_FsmDoor());
  TEST_ASSERT_EQUAL_UINT(1U, SmartLock_JournalPending());
  TEST_ASSERT_EQUAL_UINT32(1U, SmartLock_FsmIgnored());
}

static void test_camera_rejection_is_counted(void)
{
  go_locked(0U);
  cam_accept = false;   /* камери немає / зайнята */
  post(SL_EV_MOTION, 0U);
  (void)step(20000U);
  TEST_ASSERT_EQUAL_UINT32(1U, SmartLock_FsmCameraRejected());
  expect_log(0U, SL_LOG_MOTION, 0U);   /* подія в журналі є все одно */
  TEST_ASSERT_EQUAL(SL_STATE_LOCKED, SmartLock_FsmState());
}

static void test_storage_with_default_backend_counts_unsaved(void)
{
  post(SL_EV_LOCK_REQUEST, SL_LOCK_REQ_T_NOW);
  (void)step(0U);
  TEST_ASSERT_TRUE(SmartLock_StoragePoll(0U));   /* EVT,...,name=LOCK_ACTIVATED,...,flash=err */
  TEST_ASSERT_EQUAL_UINT32(1U, SmartLock_JournalUnsaved());
  TEST_ASSERT_EQUAL_UINT32(0U, SmartLock_JournalStored());
  TEST_ASSERT_FALSE(SmartLock_StoragePoll(0U));
}

/* Повний цикл PRD D.1 через Super Loop: події йдуть з черги в задачу fsm */
static void test_full_cycle_through_super_loop(void)
{
  AppTasks_Init();
  fake_platform.now_ms = 0U;
  post(SL_EV_DOOR_CLOSED, 0U);
  post(SL_EV_LOCK_REQUEST, SL_LOCK_REQ_T_NOW);
  Sched_RunOnce();
  TEST_ASSERT_EQUAL(SL_STATE_EXIT_DELAY, SmartLock_FsmState());

  for (uint32_t t = 1000U; t < 15000U; t += 1000U)
  {
    fake_platform.now_ms = t;
    Sched_RunOnce();
  }
  TEST_ASSERT_EQUAL(SL_STATE_EXIT_DELAY, SmartLock_FsmState());
  fake_platform.now_ms = 15000U;
  Sched_RunOnce();
  TEST_ASSERT_EQUAL(SL_STATE_LOCKED, SmartLock_FsmState());

  post(SL_EV_CODE_OK, 4U);
  fake_platform.now_ms = 16000U;
  Sched_RunOnce();
  TEST_ASSERT_EQUAL(SL_STATE_UNLOCKED, SmartLock_FsmState());
  /* storage забирає по одному запису за прохід — усе вже вивантажено в UART */
  for (unsigned i = 0U; i < 8U; i++) { Sched_RunOnce(); }
  TEST_ASSERT_EQUAL_UINT(0U, SmartLock_JournalPending());
  TEST_ASSERT_EQUAL_UINT32(5U, SmartLock_JournalUnsaved());   /* DOOR_CLOSED, LOCK_ACTIVATED, LOCKED, ACCESS_GRANTED, UNLOCKED */
}

int main(void)
{
  UNITY_BEGIN();
  RUN_TEST(test_init_is_unlocked_with_servo_unlock);
  RUN_TEST(test_lock_request_starts_exit_delay);
  RUN_TEST(test_door_closed_locks_exactly_at_15000_ms);
  RUN_TEST(test_exit_delay_survives_millis_overflow);
  RUN_TEST(test_countdown_starts_from_accepted_press_stamp);
  RUN_TEST(test_invalid_press_stamp_falls_back_to_now);
  RUN_TEST(test_door_open_after_delay_waits_then_locks_without_second_countdown);
  RUN_TEST(test_unknown_door_is_treated_as_open);
  RUN_TEST(test_door_opened_and_closed_during_countdown_still_locks_on_time);
  RUN_TEST(test_door_open_in_same_pass_as_deadline_prevents_lock);
  RUN_TEST(test_repeated_lock_request_does_not_restart_countdown);
  RUN_TEST(test_motion_and_tamper_in_locked_log_and_request_photo);
  RUN_TEST(test_simultaneous_motion_and_tamper_give_two_records);
  RUN_TEST(test_security_and_code_events_ignored_outside_locked);
  RUN_TEST(test_forced_open_keeps_locked_without_photo);
  RUN_TEST(test_no_forced_open_after_authorized_unlock);
  RUN_TEST(test_wrong_then_right_code);
  RUN_TEST(test_no_auto_lock_after_unlock);
  RUN_TEST(test_env_warning_logged_in_every_state_without_transition);
  RUN_TEST(test_duplicate_door_events_are_ignored);
  RUN_TEST(test_camera_rejection_is_counted);
  RUN_TEST(test_storage_with_default_backend_counts_unsaved);
  RUN_TEST(test_full_cycle_through_super_loop);
  return UNITY_END();
}
