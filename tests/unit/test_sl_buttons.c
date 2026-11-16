/**
  * @file    test_sl_buttons.c
  * @brief   ЛР3 (K03/K04): USER B1 і код з 4 кнопок — sl_buttons.c без плати.
  *
  *          Збирається з усім App/ і fake_platform (tests/Makefile, APP_TESTS).
  *          Окремий запуск:
  *            make -C tests run-sl_buttons
  *
  *          Покрито: антидребезг (короткі імпульси, межа порога), одне
  *          натискання = одна подія, утримання без автоповтору, кнопка, затиснута
  *          на старті, код (5 правильних + 5 неправильних, PRD «5+5»), тайм-аут
  *          введення, швидкі натискання, акорд, переповнення now_ms, повна черга,
  *          b1_edge разом із рівнем.
  *
  *          Час у тестах: g_now — now_ms; опитування кожні STEP мс, як
  *          TBD_PERIOD_BUTTONS_US = 10 мс. Усі тривалості виражені через TBD_*
  *          з sl_buttons.h, тож тести не прив'язані до конкретних значень.
  */
#include <string.h>

#include "unity.h"

#include "app.h"
#include "app_config.h"
#include "event_queue.h"
#include "fake_i2c.h"
#include "fake_platform.h"
#include "fake_sensors.h"
#include "platform.h"
#include "sl_buttons.h"
#include "smartlock_hooks.h"

#define STEP        10U   /* період опитування, мс */
#define B1          PLATFORM_IN_LOCK
#define ALL_BTN     (PLATFORM_IN_BTN1 | PLATFORM_IN_BTN2 | PLATFORM_IN_BTN3 | PLATFORM_IN_BTN4 | PLATFORM_IN_LOCK)

/* Скільки мс (кроками STEP) від початку натискання до прийняття: перше опитування
 * лише запам'ятовує рівень, далі ceil(DEBOUNCE / STEP) кроків */
#define ACCEPT_MS   (STEP + (((TBD_BTN_DEBOUNCE_MS + STEP - 1U) / STEP) * STEP))

static const uint8_t k_code[TBD_CODE_LEN] = TBD_CODE_VALUE;
static const uint32_t k_btn[4] =
{
  PLATFORM_IN_BTN1, PLATFORM_IN_BTN2, PLATFORM_IN_BTN3, PLATFORM_IN_BTN4
};

static uint32_t g_now;
static uint32_t g_accept;   /* now_ms, коли останній tap() був прийнятий */

void setUp(void)
{
  fake_platform_reset();
  fake_i2c_reset();
  memset(&app_devices, 0, sizeof(app_devices));
  app_devices.mpu_status  = DEV_ERR_STATE;
  app_devices.baro_status = DEV_ERR_STATE;
  SmartLock_EventsInit();
  SmartLock_ButtonsInit();
  g_now = 1000U;
  g_accept = 0U;
}

void tearDown(void)
{
}

/* ---- помічники ------------------------------------------------------------------ */
static bool poll_at(uint32_t t, bool edge)
{
  g_now = t;
  fake_platform.now_ms = t;
  return SmartLock_ButtonsPoll(t, edge);
}

static bool poll(void)
{
  return poll_at(g_now, false);
}

/* n опитувань з кроком STEP; true, якщо хоч одне повернуло true */
static bool run(uint32_t ms)
{
  bool any = false;

  for (uint32_t k = 0U; k < ms / STEP; k++)
  {
    any |= poll_at(g_now + STEP, false);
  }
  return any;
}

static void press(uint32_t mask)
{
  fake_platform_set_inputs(mask, 0);
}

static void release(uint32_t mask)
{
  fake_platform_set_inputs(mask, 1);
}

/* Перше опитування (ледача ініціалізація) при всіх відпущених кнопках */
static void start_idle(void)
{
  release(ALL_BTN);
  TEST_ASSERT_FALSE(poll());
}

/* Повне натискання: тримає ACCEPT_MS, відпускає, чекає DEBOUNCE. Повертає момент
 * прийняття в g_accept. */
static void tap(uint32_t mask)
{
  bool seen = false;

  press(mask);
  for (uint32_t k = 0U; k < ACCEPT_MS / STEP; k++)
  {
    if (poll_at(g_now + STEP, false) && !seen)
    {
      seen = true;
      g_accept = g_now;
    }
  }
  TEST_ASSERT_TRUE_MESSAGE(seen, "натискання не прийнято");
  release(mask);
  (void)run(ACCEPT_MS);
}

/* Дочекатися моменту t (опитування кожні STEP) */
static void idle_until(uint32_t t)
{
  while ((int32_t)(t - g_now) > 0)
  {
    (void)poll_at(g_now + STEP, false);
  }
}

static void expect_event(sl_event_type_t type, uint32_t arg)
{
  sl_event_t ev;

  TEST_ASSERT_TRUE_MESSAGE(SmartLock_TakeEvent(&ev), "очікувалась подія");
  TEST_ASSERT_EQUAL_STRING(SmartLock_EventName(type), SmartLock_EventName(ev.type));
  TEST_ASSERT_EQUAL_UINT32(arg, ev.arg);
}

static void expect_no_events(void)
{
  TEST_ASSERT_EQUAL_UINT(0U, SmartLock_EventsPending());
}

static void enter_code(const uint8_t *symbols, unsigned n)
{
  for (unsigned i = 0U; i < n; i++)
  {
    tap(k_btn[symbols[i] - 1U]);
  }
}

/* ---- ініціалізація, повернення ---------------------------------------------------- */
static void test_init_posts_nothing(void)
{
  SmartLock_ButtonsInit();
  TEST_ASSERT_EQUAL_UINT(0U, SmartLock_EventsPending());
  TEST_ASSERT_EQUAL_UINT32(0U, SmartLock_ButtonsDropped());
  TEST_ASSERT_EQUAL_UINT(0U, SmartLock_ButtonsCodeProgress());
}

static void test_idle_poll_does_no_useful_work(void)
{
  start_idle();
  TEST_ASSERT_FALSE(run(2000U));   /* слабка версія завжди true — тут сильна, false */
  expect_no_events();
}

/* ---- B1: антидребезг і одна подія -------------------------------------------------- */
static void test_b1_press_posts_one_lock_request_with_accept_time(void)
{
  start_idle();
  tap(B1);
  expect_event(SL_EV_LOCK_REQUEST, g_accept);
  expect_no_events();
  TEST_ASSERT_EQUAL_UINT32(1000U + ACCEPT_MS, g_accept);   /* мітка — момент прийняття, не фронту */
}

static void test_b1_poll_returns_true_on_accept_and_on_release_only(void)
{
  start_idle();
  press(B1);
  TEST_ASSERT_FALSE(run(ACCEPT_MS - STEP));   /* ще не прийнято */
  TEST_ASSERT_TRUE(run(STEP));                /* прийняття */
  TEST_ASSERT_FALSE(run(500U));               /* утримання — немає роботи */
  release(B1);
  TEST_ASSERT_FALSE(run(TBD_BTN_DEBOUNCE_MS - STEP));
  TEST_ASSERT_TRUE(run(2U * STEP));           /* прийняте відпускання — зміна стану, але без події */
  TEST_ASSERT_FALSE(run(500U));
  expect_event(SL_EV_LOCK_REQUEST, 1000U + ACCEPT_MS);
  expect_no_events();
}

static void test_b1_hold_gives_single_event_no_autorepeat(void)
{
  start_idle();
  press(B1);
  (void)run(10000U);
  expect_event(SL_EV_LOCK_REQUEST, 1000U + ACCEPT_MS);
  expect_no_events();
  release(B1);
  (void)run(ACCEPT_MS);
  expect_no_events();   /* відпускання події не дає */
}

static void test_b1_bounce_shorter_than_threshold_gives_nothing(void)
{
  start_idle();
  for (unsigned i = 0U; i < 20U; i++)
  {
    press(B1);
    (void)run(STEP);                       /* низький рівень менше порога */
    release(B1);
    (void)run(STEP);
  }
  (void)run(1000U);
  expect_no_events();
}

static void test_b1_threshold_edge_with_1ms_polling(void)
{
  start_idle();
  press(B1);
  (void)poll_at(2000U, false);   /* перший низький рівень */
  TEST_ASSERT_FALSE(poll_at(2000U + TBD_BTN_DEBOUNCE_MS - 1U, false));
  expect_no_events();
  TEST_ASSERT_TRUE(poll_at(2000U + TBD_BTN_DEBOUNCE_MS, false));
  expect_event(SL_EV_LOCK_REQUEST, 2000U + TBD_BTN_DEBOUNCE_MS);
  expect_no_events();
}

static void test_b1_bounce_burst_then_stable_press_gives_one_event(void)
{
  start_idle();
  /* серія відскоків кожні STEP, потім стабільно низький рівень */
  for (unsigned i = 0U; i < 5U; i++)
  {
    press(B1);
    (void)run(STEP);
    release(B1);
    (void)run(STEP);
  }
  press(B1);
  (void)poll_at(g_now + STEP, false);
  uint32_t t_first_low = g_now;
  (void)run(ACCEPT_MS);
  expect_event(SL_EV_LOCK_REQUEST, t_first_low + ((TBD_BTN_DEBOUNCE_MS + STEP - 1U) / STEP) * STEP);
  expect_no_events();
}

static void test_b1_release_glitch_while_held_is_ignored(void)
{
  start_idle();
  press(B1);
  (void)run(ACCEPT_MS);
  expect_event(SL_EV_LOCK_REQUEST, 1000U + ACCEPT_MS);
  release(B1);
  (void)run(STEP);      /* глітч відпускання коротший за поріг */
  press(B1);
  (void)run(ACCEPT_MS + 1000U);
  expect_no_events();   /* стабільного відпускання не було — нового натискання теж */
}

static void test_b1_two_separate_presses_two_events(void)
{
  start_idle();
  tap(B1);
  uint32_t t1 = g_accept;
  idle_until(g_now + 500U);
  tap(B1);
  expect_event(SL_EV_LOCK_REQUEST, t1);
  expect_event(SL_EV_LOCK_REQUEST, g_accept);
  expect_no_events();
}

/* ---- затиснута на старті ---------------------------------------------------------------- */
static void test_button_held_at_start_gives_no_event_until_released(void)
{
  press(B1 | PLATFORM_IN_BTN2);
  TEST_ASSERT_FALSE(poll());               /* перше опитування */
  TEST_ASSERT_FALSE(run(3000U));           /* утримання */
  expect_no_events();
  TEST_ASSERT_EQUAL_UINT(0U, SmartLock_ButtonsCodeProgress());
  release(B1 | PLATFORM_IN_BTN2);
  (void)run(ACCEPT_MS);
  expect_no_events();                      /* відпускання події не дає */
  tap(B1);                                 /* після відпускання працює як звичайно */
  expect_event(SL_EV_LOCK_REQUEST, g_accept);
}

/* ---- код: PRD «5 правильних + 5 неправильних» ----------------------------------------- */
static void test_code_ok_five_times(void)
{
  start_idle();
  for (unsigned n = 0U; n < 5U; n++)
  {
    enter_code(k_code, TBD_CODE_LEN);
    expect_event(SL_EV_CODE_OK, TBD_CODE_LEN);
    expect_no_events();
    TEST_ASSERT_EQUAL_UINT(0U, SmartLock_ButtonsCodeProgress());
  }
  TEST_ASSERT_EQUAL_UINT32(0U, SmartLock_ButtonsDropped());
}

static void test_code_wrong_five_times(void)
{
  uint8_t bad[TBD_CODE_LEN];

  start_idle();
  for (unsigned n = 0U; n < 5U; n++)
  {
    memcpy(bad, k_code, sizeof(bad));
    switch (n)
    {
    case 0U: bad[TBD_CODE_LEN - 1U] = (uint8_t)((bad[TBD_CODE_LEN - 1U] % 4U) + 1U); break;   /* лише останній */
    case 1U: bad[0] = (uint8_t)((bad[0] % 4U) + 1U); break;                                    /* лише перший */
    case 2U: bad[0] = k_code[TBD_CODE_LEN - 1U]; bad[TBD_CODE_LEN - 1U] = k_code[0]; break;     /* перестановка */
    case 3U: memset(bad, 1, sizeof(bad)); bad[0] = (k_code[0] == 1U) ? 2U : 1U; break;         /* не код */
    default: for (unsigned i = 0U; i < TBD_CODE_LEN; i++) { bad[i] = (uint8_t)((k_code[i] % 4U) + 1U); } break;
    }
    TEST_ASSERT_NOT_EQUAL(0, memcmp(bad, k_code, sizeof(bad)));
    enter_code(bad, TBD_CODE_LEN);
    expect_event(SL_EV_CODE_WRONG, TBD_CODE_LEN);
    expect_no_events();
  }
}

static void test_code_event_only_after_last_symbol(void)
{
  start_idle();
  for (unsigned i = 0U; i + 1U < TBD_CODE_LEN; i++)
  {
    tap(k_btn[k_code[i] - 1U]);
    expect_no_events();
    TEST_ASSERT_EQUAL_UINT(i + 1U, SmartLock_ButtonsCodeProgress());
  }
  tap(k_btn[k_code[TBD_CODE_LEN - 1U] - 1U]);
  expect_event(SL_EV_CODE_OK, TBD_CODE_LEN);
}

static void test_code_buttons_map_to_symbols_1_to_4(void)
{
  /* кожна кнопка дає свій символ: BTN_k у позиції 0 дає WRONG, якщо k != k_code[0],
   * і OK тільки коли весь код збігся; перевіряємо через повний код з нулем помилок */
  start_idle();
  for (unsigned b = 0U; b < 4U; b++)
  {
    uint8_t seq[TBD_CODE_LEN];

    memcpy(seq, k_code, sizeof(seq));
    seq[0] = (uint8_t)(b + 1U);
    enter_code(seq, TBD_CODE_LEN);
    expect_event((seq[0] == k_code[0]) ? SL_EV_CODE_OK : SL_EV_CODE_WRONG, TBD_CODE_LEN);
  }
}

static void test_code_too_many_presses_evaluates_each_block(void)
{
  start_idle();
  enter_code(k_code, TBD_CODE_LEN);
  enter_code(k_code, 2U);
  expect_event(SL_EV_CODE_OK, TBD_CODE_LEN);
  expect_no_events();
  TEST_ASSERT_EQUAL_UINT(2U, SmartLock_ButtonsCodeProgress());   /* надлишок — початок нового введення */
}

static void test_code_hold_gives_single_symbol(void)
{
  start_idle();
  press(k_btn[0]);
  (void)run(TBD_CODE_TIMEOUT_MS / 2U);
  TEST_ASSERT_EQUAL_UINT(1U, SmartLock_ButtonsCodeProgress());   /* без автоповтору */
  (void)run(2U * TBD_CODE_TIMEOUT_MS);
  TEST_ASSERT_EQUAL_UINT(0U, SmartLock_ButtonsCodeProgress());   /* тайм-аут скинув, утримання нового символу не дало */
  expect_no_events();
}

/* ---- тайм-аут введення -------------------------------------------------------------------- */
static void test_code_timeout_resets_partial_entry_without_event(void)
{
  bool did;

  start_idle();
  enter_code(k_code, TBD_CODE_LEN - 1U);
  TEST_ASSERT_EQUAL_UINT(TBD_CODE_LEN - 1U, SmartLock_ButtonsCodeProgress());
  idle_until(g_accept + TBD_CODE_TIMEOUT_MS - STEP);
  TEST_ASSERT_EQUAL_UINT(TBD_CODE_LEN - 1U, SmartLock_ButtonsCodeProgress());   /* ще не прострочено */
  did = poll_at(g_accept + TBD_CODE_TIMEOUT_MS, false);
  TEST_ASSERT_TRUE(did);                                                         /* скидання — корисна робота */
  TEST_ASSERT_EQUAL_UINT(0U, SmartLock_ButtonsCodeProgress());
  TEST_ASSERT_FALSE(run(500U));
  expect_no_events();
  /* після скидання введення починається спочатку і завершується правильно */
  enter_code(k_code, TBD_CODE_LEN);
  expect_event(SL_EV_CODE_OK, TBD_CODE_LEN);
  expect_no_events();
}

static void test_code_pause_just_below_timeout_keeps_entry(void)
{
  start_idle();
  enter_code(k_code, 1U);
  uint32_t t_first = g_accept;
  /* наступне натискання прийнято за TIMEOUT - STEP після першого */
  idle_until(t_first + TBD_CODE_TIMEOUT_MS - STEP - ACCEPT_MS);
  press(k_btn[k_code[1] - 1U]);
  for (uint32_t k = 0U; k < ACCEPT_MS / STEP; k++)
  {
    (void)poll_at(g_now + STEP, false);
  }
  TEST_ASSERT_EQUAL_UINT32(t_first + TBD_CODE_TIMEOUT_MS - STEP, g_now);
  TEST_ASSERT_EQUAL_UINT(2U, SmartLock_ButtonsCodeProgress());
}

static void test_code_press_after_timeout_starts_new_entry(void)
{
  start_idle();
  enter_code(k_code, 2U);
  idle_until(g_accept + TBD_CODE_TIMEOUT_MS + 2U * STEP);
  TEST_ASSERT_EQUAL_UINT(0U, SmartLock_ButtonsCodeProgress());
  enter_code(k_code, 1U);
  TEST_ASSERT_EQUAL_UINT(1U, SmartLock_ButtonsCodeProgress());
  expect_no_events();
}

/* ---- швидкі натискання ------------------------------------------------------------------ */
static void test_rapid_taps_are_all_counted(void)
{
  start_idle();
  /* відпускання рівно DEBOUNCE між натисканнями, утримання рівно DEBOUNCE */
  for (unsigned i = 0U; i < TBD_CODE_LEN; i++)
  {
    uint32_t mask = k_btn[k_code[i] - 1U];

    press(mask);
    (void)run(((TBD_BTN_DEBOUNCE_MS + STEP - 1U) / STEP) * STEP + STEP);
    release(mask);
    (void)run(((TBD_BTN_DEBOUNCE_MS + STEP - 1U) / STEP) * STEP + STEP);
  }
  expect_event(SL_EV_CODE_OK, TBD_CODE_LEN);
  expect_no_events();
}

static void test_rapid_two_buttons_without_release_between(void)
{
  start_idle();
  press(k_btn[k_code[0] - 1U]);
  (void)run(ACCEPT_MS);
  press(k_btn[k_code[1] - 1U]);   /* перша ще затиснута: друга — окреме (послідовне) натискання */
  (void)run(ACCEPT_MS);
  TEST_ASSERT_EQUAL_UINT(2U, SmartLock_ButtonsCodeProgress());
}

/* ---- одночасні кнопки ------------------------------------------------------------------- */
static void test_chord_of_two_code_buttons_is_ignored(void)
{
  start_idle();
  press(k_btn[0] | k_btn[1]);
  (void)run(ACCEPT_MS);
  TEST_ASSERT_EQUAL_UINT(0U, SmartLock_ButtonsCodeProgress());
  expect_no_events();
  release(k_btn[0] | k_btn[1]);
  (void)run(ACCEPT_MS);
  TEST_ASSERT_EQUAL_UINT(0U, SmartLock_ButtonsCodeProgress());
}

static void test_chord_inside_entry_does_not_corrupt_it(void)
{
  start_idle();
  enter_code(k_code, 2U);
  press(k_btn[0] | k_btn[3]);
  (void)run(ACCEPT_MS);
  release(k_btn[0] | k_btn[3]);
  (void)run(ACCEPT_MS);
  TEST_ASSERT_EQUAL_UINT(2U, SmartLock_ButtonsCodeProgress());   /* акорд не змінив введення */
  enter_code(&k_code[2], TBD_CODE_LEN - 2U);
  expect_event(SL_EV_CODE_OK, TBD_CODE_LEN);
  expect_no_events();
}

static void test_chord_three_buttons_ignored_too(void)
{
  start_idle();
  press(k_btn[0] | k_btn[1] | k_btn[2]);
  (void)run(ACCEPT_MS);
  TEST_ASSERT_EQUAL_UINT(0U, SmartLock_ButtonsCodeProgress());
  expect_no_events();
}

static void test_b1_with_code_button_simultaneously_handled_independently(void)
{
  start_idle();
  press(B1 | k_btn[1]);
  (void)run(ACCEPT_MS);
  expect_event(SL_EV_LOCK_REQUEST, 1000U + ACCEPT_MS);
  expect_no_events();
  TEST_ASSERT_EQUAL_UINT(1U, SmartLock_ButtonsCodeProgress());   /* B1 не акорд з кодовою кнопкою */
}

/* ---- переповнення now_ms ------------------------------------------------------------------ */
static void test_b1_press_across_now_ms_wrap(void)
{
  g_now = 0xFFFFFFF0U;
  start_idle();
  press(B1);
  (void)run(ACCEPT_MS);   /* прийняття після переходу через 0 */
  TEST_ASSERT_TRUE((uint32_t)(g_now - 0xFFFFFFF0U) == ACCEPT_MS);
  expect_event(SL_EV_LOCK_REQUEST, g_now);
  expect_no_events();
  release(B1);
  (void)run(ACCEPT_MS);
  tap(B1);
  expect_event(SL_EV_LOCK_REQUEST, g_accept);
}

static void test_debounce_across_wrap_still_rejects_short_pulse(void)
{
  g_now = 0xFFFFFFF0U;
  start_idle();
  press(B1);
  (void)run(STEP);
  release(B1);
  (void)run(1000U);
  expect_no_events();
}

static void test_code_timeout_across_wrap(void)
{
  g_now = 0xFFFFFFFFU - 2000U;
  start_idle();
  enter_code(k_code, 2U);
  TEST_ASSERT_EQUAL_UINT(2U, SmartLock_ButtonsCodeProgress());
  idle_until(g_accept + TBD_CODE_TIMEOUT_MS - STEP);
  TEST_ASSERT_EQUAL_UINT(2U, SmartLock_ButtonsCodeProgress());
  (void)poll_at(g_accept + TBD_CODE_TIMEOUT_MS, false);
  TEST_ASSERT_EQUAL_UINT(0U, SmartLock_ButtonsCodeProgress());
  TEST_ASSERT_TRUE(g_now < 0x80000000U);   /* час справді переходив через 0 */
}

/* ---- повна черга ------------------------------------------------------------------------ */
static void fill_queue(void)
{
  for (unsigned i = 0U; i < TBD_EVQ_LEN; i++)
  {
    TEST_ASSERT_TRUE(SmartLock_PostEvent(SL_EV_MOTION, 0U));
  }
  TEST_ASSERT_FALSE(SmartLock_PostEvent(SL_EV_MOTION, 0U));   /* заповнено; свій drop рахує черга */
}

static void test_full_queue_counts_dropped_lock_request(void)
{
  sl_event_t ev;

  start_idle();
  fill_queue();
  tap(B1);
  TEST_ASSERT_EQUAL_UINT32(1U, SmartLock_ButtonsDropped());
  while (SmartLock_TakeEvent(&ev))
  {
    TEST_ASSERT_EQUAL(SL_EV_MOTION, ev.type);   /* LOCK_REQUEST у чергу не потрапила */
  }
  tap(B1);   /* черга вільна — нове натискання проходить */
  expect_event(SL_EV_LOCK_REQUEST, g_accept);
  TEST_ASSERT_EQUAL_UINT32(1U, SmartLock_ButtonsDropped());
}

static void test_full_queue_counts_dropped_code_event_and_clears_entry(void)
{
  start_idle();
  fill_queue();
  enter_code(k_code, TBD_CODE_LEN);
  TEST_ASSERT_EQUAL_UINT32(1U, SmartLock_ButtonsDropped());
  TEST_ASSERT_EQUAL_UINT(0U, SmartLock_ButtonsCodeProgress());   /* введення не «зависло» */
  SmartLock_ButtonsInit();
  TEST_ASSERT_EQUAL_UINT32(0U, SmartLock_ButtonsDropped());      /* Init скидає лічильник */
}

/* ---- b1_edge разом із рівнем ------------------------------------------------------------- */
static void test_b1_edge_with_level_gives_single_event(void)
{
  start_idle();
  press(B1);
  (void)poll_at(g_now + STEP, true);    /* фронт EXTI у тому ж проході, що й низький рівень */
  for (uint32_t k = 0U; k < 100U; k++)
  {
    (void)poll_at(g_now + STEP, false);
  }
  expect_event(SL_EV_LOCK_REQUEST, 1000U + STEP + ((TBD_BTN_DEBOUNCE_MS + STEP - 1U) / STEP) * STEP);
  expect_no_events();
}

static void test_b1_edge_while_released_gives_nothing(void)
{
  start_idle();
  (void)poll_at(g_now + STEP, true);   /* завада: фронт був, рівень уже високий */
  (void)run(1000U);
  expect_no_events();
}

static void test_b1_edge_restarts_debounce(void)
{
  uint32_t t2;

  start_idle();
  press(B1);
  (void)poll_at(g_now + STEP, false);   /* T1: перший низький рівень */
  t2 = g_now + STEP;
  (void)poll_at(t2, true);              /* T2: ще фронт — відлік стабільності заново */
  /* без перезапуску прийняття було б уже тут (T1 + DEBOUNCE) */
  TEST_ASSERT_FALSE(poll_at(t2 + TBD_BTN_DEBOUNCE_MS - STEP, false));
  expect_no_events();
  TEST_ASSERT_TRUE(poll_at(t2 + TBD_BTN_DEBOUNCE_MS, false));
  expect_event(SL_EV_LOCK_REQUEST, t2 + TBD_BTN_DEBOUNCE_MS);
  expect_no_events();
}

static void test_b1_edge_on_release_gives_no_extra_event(void)
{
  start_idle();
  press(B1);
  (void)run(ACCEPT_MS);
  expect_event(SL_EV_LOCK_REQUEST, 1000U + ACCEPT_MS);
  release(B1);
  (void)poll_at(g_now + STEP, true);   /* фронт відпускання */
  (void)run(ACCEPT_MS);
  expect_no_events();
}

int main(void)
{
  UNITY_BEGIN();
  RUN_TEST(test_init_posts_nothing);
  RUN_TEST(test_idle_poll_does_no_useful_work);
  RUN_TEST(test_b1_press_posts_one_lock_request_with_accept_time);
  RUN_TEST(test_b1_poll_returns_true_on_accept_and_on_release_only);
  RUN_TEST(test_b1_hold_gives_single_event_no_autorepeat);
  RUN_TEST(test_b1_bounce_shorter_than_threshold_gives_nothing);
  RUN_TEST(test_b1_threshold_edge_with_1ms_polling);
  RUN_TEST(test_b1_bounce_burst_then_stable_press_gives_one_event);
  RUN_TEST(test_b1_release_glitch_while_held_is_ignored);
  RUN_TEST(test_b1_two_separate_presses_two_events);
  RUN_TEST(test_button_held_at_start_gives_no_event_until_released);
  RUN_TEST(test_code_ok_five_times);
  RUN_TEST(test_code_wrong_five_times);
  RUN_TEST(test_code_event_only_after_last_symbol);
  RUN_TEST(test_code_buttons_map_to_symbols_1_to_4);
  RUN_TEST(test_code_too_many_presses_evaluates_each_block);
  RUN_TEST(test_code_hold_gives_single_symbol);
  RUN_TEST(test_code_timeout_resets_partial_entry_without_event);
  RUN_TEST(test_code_pause_just_below_timeout_keeps_entry);
  RUN_TEST(test_code_press_after_timeout_starts_new_entry);
  RUN_TEST(test_rapid_taps_are_all_counted);
  RUN_TEST(test_rapid_two_buttons_without_release_between);
  RUN_TEST(test_chord_of_two_code_buttons_is_ignored);
  RUN_TEST(test_chord_inside_entry_does_not_corrupt_it);
  RUN_TEST(test_chord_three_buttons_ignored_too);
  RUN_TEST(test_b1_with_code_button_simultaneously_handled_independently);
  RUN_TEST(test_b1_press_across_now_ms_wrap);
  RUN_TEST(test_debounce_across_wrap_still_rejects_short_pulse);
  RUN_TEST(test_code_timeout_across_wrap);
  RUN_TEST(test_full_queue_counts_dropped_lock_request);
  RUN_TEST(test_full_queue_counts_dropped_code_event_and_clears_entry);
  RUN_TEST(test_b1_edge_with_level_gives_single_event);
  RUN_TEST(test_b1_edge_while_released_gives_nothing);
  RUN_TEST(test_b1_edge_restarts_debounce);
  RUN_TEST(test_b1_edge_on_release_gives_no_extra_event);
  return UNITY_END();
}
