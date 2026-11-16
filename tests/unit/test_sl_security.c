/**
  * @file    test_sl_security.c
  * @brief   ЛР3 (K05): PIR, MPU-6050, геркон — sl_security.c без плати.
  *
  *          Збирається з усім App/ і fake_platform (tests/Makefile, APP_TESTS).
  *          Окремий запуск: make -C tests run-sl_security
  *
  *          Геркон: початкова подія для обох станів, дребезг і короткі імпульси,
  *          затримка рівно TBD_DOOR_DEBOUNCE_MS, повтор DOOR_* при повній черзі.
  *          PIR: фронт, утримання рівня, серія імпульсів HC-SR501, pir_edge.
  *          MPU-6050: шум зі спокою (logs/09, 11) без TAMPER, поворот руками
  *          (logs/11, інтерполяція до 20 мс), стрибок = один TAMPER із потрібним
  *          arg; відмови: mpu_status, помилка і серія помилок I2C, нульові осі.
  *          Переповнення Platform_Millis() — для всіх трьох інтервалів.
  *
  *          Залізо: fake_platform (входи PLATFORM_IN_*), fake_sensors (MPU-6050
  *          через app_devices), fake_i2c (помилки шини). Події — через
  *          SmartLock_TakeEvent().
  */
#include <string.h>

#include "unity.h"

#include "app.h"
#include "event_queue.h"
#include "fake_i2c.h"
#include "fake_platform.h"
#include "fake_sensors.h"
#include "sl_security.h"
#include "smartlock_hooks.h"

#define STEP_MS 20U   /* TBD_PERIOD_SECURITY_US */

/* Шум акселерометра у спокої, мg: logs/09 (92 виміри, 104 с, модуль на ребрі) і
 * logs/11 (44 виміри після повороту, модуль рівно) */
static const int16_t NOISE_L09_MG[][3] =
{
  { -932, -5, -3 }, { -929, -3, -2 }, { -926, -2, -9 }, { -935, -8, -4 },
  { -937, -2, 3 }, { -934, -7, -1 }, { -937, -6, 0 }, { -931, -3, -6 },
  { -927, -1, 9 }, { -927, -9, -3 }, { -932, -2, -10 }, { -931, -3, -3 },
  { -934, -7, -9 }, { -934, 0, -13 }, { -925, -5, 1 }, { -927, -8, -1 },
  { -927, -4, -7 }, { -929, -3, -1 }, { -931, -5, -1 }, { -930, -4, 0 },
  { -931, 3, -1 }, { -935, 0, 0 }, { -933, -5, -9 }, { -924, -9, 0 },
  { -933, 0, -6 }, { -928, -1, -10 }, { -926, -3, 0 }, { -926, 2, 0 },
  { -928, -3, -10 }, { -934, -1, -5 }, { -933, -5, 4 }, { -936, 0, -4 },
  { -934, -12, -3 }, { -934, -6, -11 }, { -927, -4, -4 }, { -929, -10, -12 },
  { -928, -2, -11 }, { -931, -2, 3 }, { -933, -9, 0 }, { -930, -3, 3 },
  { -931, -3, -1 }, { -929, -5, 0 }, { -928, -5, 3 }, { -926, -3, 1 },
  { -931, -5, 0 }, { -935, -8, 1 }, { -929, -5, 3 }, { -929, -6, -3 },
  { -935, -9, -7 }, { -928, -10, 2 }, { -934, -2, 3 }, { -931, -9, 0 },
  { -933, 2, -8 }, { -932, -5, -5 }, { -936, -9, -6 }, { -934, -5, 0 },
  { -926, 0, 2 }, { -936, -3, -1 }, { -931, 2, 2 }, { -932, -7, 4 },
  { -933, -7, 0 }, { -935, -5, -5 }, { -927, -2, -3 }, { -936, -5, -4 },
  { -933, -10, 0 }, { -937, 0, 5 }, { -933, -3, -7 }, { -935, -1, -1 },
  { -935, -3, -5 }, { -936, -3, 0 }, { -930, -3, -3 }, { -927, -9, 1 },
  { -926, -1, -1 }, { -930, 2, 5 }, { -929, -8, -7 }, { -932, -2, -2 },
  { -928, -4, -8 }, { -932, -4, -6 }, { -933, 0, -1 }, { -927, -6, 0 },
  { -928, -3, -4 }, { -928, 1, -13 }, { -931, -5, -9 }, { -932, -5, -10 },
  { -929, -3, 0 }, { -937, -2, -5 }, { -927, -3, -5 }, { -933, -5, -9 },
  { -934, -8, 0 }, { -932, -7, 1 }, { -933, 0, -5 }, { -926, -10, 10 },
};

static const int16_t NOISE_L11_MG[][3] =
{
  { 45, -25, 1101 }, { 47, -17, 1112 }, { 46, -15, 1103 }, { 47, -15, 1106 },
  { 49, -13, 1096 }, { 52, -17, 1107 }, { 49, -15, 1097 }, { 49, -12, 1101 },
  { 55, -15, 1102 }, { 56, -16, 1100 }, { 54, -18, 1102 }, { 49, -9, 1104 },
  { 50, -13, 1105 }, { 55, -17, 1094 }, { 42, -17, 1099 }, { 52, -21, 1105 },
  { 49, -16, 1091 }, { 59, -11, 1098 }, { 56, -17, 1102 }, { 42, -14, 1100 },
  { 44, -15, 1102 }, { 52, -10, 1107 }, { 56, -19, 1095 }, { 55, -20, 1100 },
  { 57, -9, 1087 }, { 52, -12, 1101 }, { 51, -20, 1103 }, { 54, -22, 1100 },
  { 45, -21, 1103 }, { 45, -11, 1099 }, { 43, -21, 1107 }, { 46, -7, 1109 },
  { 48, -16, 1104 }, { 49, -16, 1095 }, { 53, -13, 1108 }, { 52, -14, 1090 },
  { 52, -13, 1100 }, { 53, -18, 1106 }, { 54, -17, 1101 }, { 50, -16, 1105 },
  { 55, -18, 1100 }, { 56, -15, 1104 }, { 53, -18, 1109 }, { 52, -17, 1101 },
};

/* Поворот макета руками, logs/11, 19,5–27,4 с (мg; між зразками 1,1 с) */
static const int16_t ROTATION_L11_MG[][3] =
{
  { 998, 15, 390 }, { 723, 0, 578 }, { 68, -25, 1104 }, { 94, 798, 714 }, { 126, 995, 300 },
  { 102, 930, 498 }, { 151, 16, 1043 }, { 55, -21, 1104 },
};

void setUp(void)
{
  fake_platform_reset();
  fake_i2c_reset();
  memset(&app_devices, 0, sizeof(app_devices));
  app_devices.mpu_status  = DEV_ERR_STATE;
  app_devices.baro_status = DEV_ERR_STATE;
  SmartLock_EventsInit();
  SmartLock_SecurityInit();
}

void tearDown(void)
{
}

/* ---- помічники --------------------------------------------------------------------- */
static bool poll(uint32_t now)
{
  fake_platform.now_ms = now;
  return SmartLock_SecurityPoll(now, false, false);
}

/* n опитувань з кроком 20 мс; *t — час наступного опитування */
static void run(uint32_t *t, unsigned n)
{
  for (unsigned i = 0U; i < n; i++)
  {
    (void)poll(*t);
    *t += STEP_MS;
  }
}

static void expect_ev(sl_event_type_t type, uint32_t arg)
{
  sl_event_t ev;

  TEST_ASSERT_TRUE_MESSAGE(SmartLock_TakeEvent(&ev), "очікувалась подія, черга порожня");
  TEST_ASSERT_EQUAL_STRING(SmartLock_EventName(type), SmartLock_EventName(ev.type));
  TEST_ASSERT_EQUAL_UINT32(arg, ev.arg);
}

static void expect_none(void)
{
  TEST_ASSERT_EQUAL_UINT(0U, SmartLock_EventsPending());
}

/* Забирає всю чергу; рахує події за типами, arg останньої події кожного типу */
static unsigned cnt[SL_EV_COUNT];
static uint32_t last_arg[SL_EV_COUNT];

static void drain(void)
{
  sl_event_t ev;

  memset(cnt, 0, sizeof(cnt));
  memset(last_arg, 0, sizeof(last_arg));
  while (SmartLock_TakeEvent(&ev))
  {
    cnt[ev.type]++;
    last_arg[ev.type] = ev.arg;
  }
}

static void door_closed(void)
{
  fake_platform_set_inputs(PLATFORM_IN_DOOR, 0);   /* магніт поруч */
}

static void door_open(void)
{
  fake_platform_set_inputs(PLATFORM_IN_DOOR, 1);
}

static void pir(int high)
{
  fake_platform_set_inputs(PLATFORM_IN_PIR, high);
}

/* ±2 g: значення мають бути в межах ±1999 мg (int16 LSB) */
static void accel_mg(int mg_x, int mg_y, int mg_z)
{
  fake_sensors_mpu_set_accel((int16_t)((mg_x * 16384) / 1000), (int16_t)((mg_y * 16384) / 1000),
                             (int16_t)((mg_z * 16384) / 1000));
}

/* MPU підключено, спокій (0, 0, 1 g), базова лінія встановлена: наступне опитування
 * вже порівнюється з нею. Двері (відчинені) — подія DOOR_OPEN знята з черги. */
static uint32_t start_mpu_at(uint32_t t, int mg_x, int mg_y, int mg_z)
{
  TEST_ASSERT_EQUAL(DEV_OK, fake_sensors_mpu_attach());
  accel_mg(mg_x, mg_y, mg_z);
  run(&t, TBD_MPU_BASELINE_SAMPLES);
  run(&t, (TBD_DOOR_DEBOUNCE_MS / STEP_MS) + 1U);
  drain();
  TEST_ASSERT_EQUAL_UINT(1U, cnt[SL_EV_DOOR_OPEN]);
  TEST_ASSERT_EQUAL_UINT(0U, cnt[SL_EV_TAMPER]);
  return t;
}

static uint32_t start_mpu(void)
{
  return start_mpu_at(0U, 0, 0, 1000);
}

/* Кладе таблицю зразків (мg) в акселерометр по одному на опитування */
static void feed(const int16_t tbl[][3], unsigned n, uint32_t *t)
{
  for (unsigned i = 0U; i < n; i++)
  {
    accel_mg(tbl[i][0], tbl[i][1], tbl[i][2]);
    (void)poll(*t);
    *t += STEP_MS;
  }
}

static void fill_queue(void)
{
  while (SmartLock_PostEvent(SL_EV_ENV_WARNING, 0U))
  {
  }
}

/* ---- init ------------------------------------------------------------------------------ */
static void test_init_posts_nothing(void)
{
  SmartLock_SecurityInit();
  TEST_ASSERT_EQUAL_UINT(0U, SmartLock_EventsPending());
  TEST_ASSERT_EQUAL_UINT32(0U, SlSecurity_EventsLost());
  TEST_ASSERT_EQUAL_UINT32(0U, SlSecurity_DoorRetries());
  TEST_ASSERT_EQUAL_UINT32(0U, SlSecurity_MpuErrors());
  TEST_ASSERT_EQUAL_UINT32(0U, SlSecurity_MpuZeroSamples());
}

static void test_init_resets_door_state_and_counters(void)
{
  uint32_t t = 0U;

  door_closed();
  run(&t, 6U);
  expect_ev(SL_EV_DOOR_CLOSED, 0U);
  fill_queue();
  pir(1);
  (void)poll(t);
  pir(0);
  (void)poll(t + 20U);
  pir(1);
  (void)poll(t + 40U);
  TEST_ASSERT_TRUE(SlSecurity_EventsLost() > 0U);

  SmartLock_SecurityInit();
  drain();
  TEST_ASSERT_EQUAL_UINT32(0U, SlSecurity_EventsLost());
  t = 10000U;
  run(&t, 6U);   /* початковий стан фіксується знову */
  expect_ev(SL_EV_DOOR_CLOSED, 0U);
}

/* ---- геркон ----------------------------------------------------------------------------- */
static void test_door_initial_closed_after_debounce(void)
{
  door_closed();
  TEST_ASSERT_FALSE(poll(0U));
  TEST_ASSERT_FALSE(poll(TBD_DOOR_DEBOUNCE_MS - 1U));
  expect_none();
  TEST_ASSERT_TRUE(poll(TBD_DOOR_DEBOUNCE_MS));
  expect_ev(SL_EV_DOOR_CLOSED, 0U);
  expect_none();

  TEST_ASSERT_FALSE(poll(TBD_DOOR_DEBOUNCE_MS + 20U));   /* вдруге не повторюється */
  TEST_ASSERT_FALSE(poll(60000U));
  expect_none();
}

static void test_door_initial_open_after_debounce(void)
{
  door_open();
  (void)poll(0U);
  (void)poll(TBD_DOOR_DEBOUNCE_MS - 1U);
  expect_none();
  TEST_ASSERT_TRUE(poll(TBD_DOOR_DEBOUNCE_MS));
  expect_ev(SL_EV_DOOR_OPEN, 0U);
  (void)poll(5000U);
  expect_none();
}

static void test_door_chatter_before_first_state(void)
{
  door_closed();
  (void)poll(0U);
  door_open();
  (void)poll(20U);
  door_closed();
  (void)poll(40U);   /* стабільний рівень рахується від останньої зміни */
  (void)poll(40U + TBD_DOOR_DEBOUNCE_MS - 1U);
  expect_none();
  (void)poll(40U + TBD_DOOR_DEBOUNCE_MS);
  expect_ev(SL_EV_DOOR_CLOSED, 0U);
  expect_none();
}

static void test_door_bounce_and_short_pulses_give_no_events(void)
{
  uint32_t t = 200U;

  door_closed();
  (void)poll(0U);
  (void)poll(TBD_DOOR_DEBOUNCE_MS);
  expect_ev(SL_EV_DOOR_CLOSED, 0U);

  /* дребезг: рівень міняється кожні 10 мс упродовж 300 мс, закінчується зачиненим */
  for (unsigned i = 0U; i < 30U; i++)
  {
    if ((i % 2U) == 0U)
    {
      door_open();
    }
    else
    {
      door_closed();
    }
    (void)poll(t);
    t += 10U;
  }
  door_closed();
  (void)poll(t);
  (void)poll(t + 500U);
  expect_none();

  /* імпульс «відчинено» коротший за антидребезг */
  t = 5000U;
  door_open();
  (void)poll(t);
  (void)poll(t + TBD_DOOR_DEBOUNCE_MS - 1U);
  door_closed();
  (void)poll(t + TBD_DOOR_DEBOUNCE_MS);
  (void)poll(t + 2U * TBD_DOOR_DEBOUNCE_MS);
  (void)poll(t + 10000U);
  expect_none();

  /* і зачинено — коротший імпульс уже відчиненого стану */
  t = 20000U;
  door_open();
  (void)poll(t);
  (void)poll(t + TBD_DOOR_DEBOUNCE_MS);
  expect_ev(SL_EV_DOOR_OPEN, 0U);
  door_closed();
  (void)poll(t + 1000U);
  door_open();
  (void)poll(t + 1000U + 60U);
  (void)poll(t + 3000U);
  expect_none();
}

static void test_door_open_and_close_events(void)
{
  uint32_t t = 0U;

  door_closed();
  (void)poll(t);
  (void)poll(t + TBD_DOOR_DEBOUNCE_MS);
  expect_ev(SL_EV_DOOR_CLOSED, 0U);

  t = 3000U;   /* відкрили */
  door_open();
  (void)poll(t);
  (void)poll(t + TBD_DOOR_DEBOUNCE_MS - 1U);
  expect_none();
  (void)poll(t + TBD_DOOR_DEBOUNCE_MS);
  expect_ev(SL_EV_DOOR_OPEN, 0U);
  expect_none();

  t = 9000U;   /* зачинили */
  door_closed();
  (void)poll(t);
  (void)poll(t + TBD_DOOR_DEBOUNCE_MS - 1U);
  expect_none();
  (void)poll(t + TBD_DOOR_DEBOUNCE_MS);
  expect_ev(SL_EV_DOOR_CLOSED, 0U);
  expect_none();
}

static void test_door_queue_full_retries_until_posted(void)
{
  sl_event_t ev;
  uint32_t   t = 0U;

  door_open();
  (void)poll(t);
  fill_queue();
  TEST_ASSERT_FALSE(poll(TBD_DOOR_DEBOUNCE_MS));   /* подію не поставлено */
  TEST_ASSERT_EQUAL_UINT32(1U, SlSecurity_DoorRetries());
  TEST_ASSERT_FALSE(poll(TBD_DOOR_DEBOUNCE_MS + 20U));   /* черга ще повна */
  TEST_ASSERT_FALSE(poll(TBD_DOOR_DEBOUNCE_MS + 40U));
  TEST_ASSERT_EQUAL_UINT32(3U, SlSecurity_DoorRetries());
  TEST_ASSERT_EQUAL_UINT32(0U, SlSecurity_EventsLost());   /* DOOR не «втрачена» */

  TEST_ASSERT_TRUE(SmartLock_TakeEvent(&ev));   /* звільнилось одне місце */
  TEST_ASSERT_TRUE(poll(TBD_DOOR_DEBOUNCE_MS + 60U));
  TEST_ASSERT_EQUAL_UINT32(3U, SlSecurity_DoorRetries());
  drain();
  TEST_ASSERT_EQUAL_UINT(1U, cnt[SL_EV_DOOR_OPEN]);
  (void)poll(TBD_DOOR_DEBOUNCE_MS + 80U);   /* вдруге не з'являється */
  expect_none();
}

static void test_door_pending_keeps_order_of_new_transition(void)
{
  uint32_t t = 0U;

  door_open();
  (void)poll(t);
  fill_queue();
  (void)poll(TBD_DOOR_DEBOUNCE_MS);   /* OPEN підтверджено, але не поставлено */
  door_closed();
  for (t = 200U; t < 600U; t += STEP_MS)
  {
    (void)poll(t);   /* CLOSED підтверджується, але чекає на OPEN */
  }
  drain();
  TEST_ASSERT_EQUAL_UINT(0U, cnt[SL_EV_DOOR_OPEN]);
  TEST_ASSERT_EQUAL_UINT(0U, cnt[SL_EV_DOOR_CLOSED]);

  TEST_ASSERT_TRUE(poll(600U));
  expect_ev(SL_EV_DOOR_OPEN, 0U);
  expect_none();
  TEST_ASSERT_TRUE(poll(620U));
  expect_ev(SL_EV_DOOR_CLOSED, 0U);
  expect_none();
}

static void test_door_pending_then_state_returns(void)
{
  uint32_t t;

  door_open();
  (void)poll(0U);
  fill_queue();
  (void)poll(TBD_DOOR_DEBOUNCE_MS);   /* OPEN у pending */
  door_closed();
  (void)poll(200U);
  door_open();   /* повернулось до відчиненого, поки OPEN ще не доставлено */
  for (t = 220U; t < 500U; t += STEP_MS)
  {
    (void)poll(t);
  }
  drain();
  (void)poll(500U);
  expect_ev(SL_EV_DOOR_OPEN, 0U);
  (void)poll(520U);
  (void)poll(2000U);
  expect_none();   /* стан збігається з підтвердженим — подій більше немає */
}

/* ---- PIR -------------------------------------------------------------------------------- */
static void test_pir_rising_edge_gives_motion(void)
{
  door_open();
  (void)poll(0U);
  (void)poll(TBD_DOOR_DEBOUNCE_MS);
  drain();

  pir(1);
  TEST_ASSERT_TRUE(poll(1000U));
  expect_ev(SL_EV_MOTION, 0U);
  expect_none();
}

static void test_pir_held_level_and_falling_edge_give_nothing(void)
{
  door_open();
  (void)poll(0U);
  (void)poll(TBD_DOOR_DEBOUNCE_MS);
  drain();

  pir(1);
  (void)poll(1000U);
  expect_ev(SL_EV_MOTION, 0U);
  for (uint32_t t = 1020U; t < 20000U; t += 500U)
  {
    (void)poll(t);   /* рівень утримується, інтервал давно минув — подій немає */
  }
  expect_none();
  pir(0);
  (void)poll(20000U);
  (void)poll(20020U);
  expect_none();
}

static void test_pir_high_at_start_is_baseline_only(void)
{
  door_open();
  pir(1);   /* HC-SR501 прогрівається і тримає вихід високим */
  (void)poll(0U);
  (void)poll(20U);
  (void)poll(TBD_DOOR_DEBOUNCE_MS);
  drain();
  TEST_ASSERT_EQUAL_UINT(0U, cnt[SL_EV_MOTION]);

  pir(0);
  (void)poll(2000U);
  pir(1);
  (void)poll(2020U);
  drain();
  TEST_ASSERT_EQUAL_UINT(1U, cnt[SL_EV_MOTION]);
}

static void test_pir_pulse_series_merged_by_min_interval(void)
{
  /* імпульси кожні ~3–6 с (logs/03, 04): події не частіше TBD_PIR_MIN_INTERVAL_MS */
  static const uint32_t rises[] = { 1000U, 4100U, 8000U, 9100U, 12000U, 16000U, 21000U };
  unsigned n = 0U;

  door_open();
  (void)poll(0U);
  (void)poll(TBD_DOOR_DEBOUNCE_MS);
  drain();

  for (unsigned i = 0U; i < (sizeof(rises) / sizeof(rises[0])); i++)
  {
    pir(0);
    (void)poll(rises[i] - 20U);
    pir(1);
    (void)poll(rises[i]);
    pir(0);
    (void)poll(rises[i] + 1000U);   /* імпульс HC-SR501 ~1 с */
    drain();
    n += cnt[SL_EV_MOTION];
  }
  /* події: 1000 (так), 4100 і 8000 (менше 8 с від першої — ні), 9100 (так: 8100 мс
   * після 1000), 12000 і 16000 (ні), 21000 (так: 11900 мс після 9100) */
  TEST_ASSERT_EQUAL_UINT(3U, n);
}

static void test_pir_min_interval_boundary(void)
{
  door_open();
  (void)poll(0U);
  (void)poll(TBD_DOOR_DEBOUNCE_MS);
  drain();

  /* на 1 мс раніше межі — ні */
  pir(1);
  (void)poll(1000U);
  pir(0);
  (void)poll(1020U);
  pir(1);
  (void)poll(1000U + TBD_PIR_MIN_INTERVAL_MS - 1U);
  drain();
  TEST_ASSERT_EQUAL_UINT(1U, cnt[SL_EV_MOTION]);

  /* рівно на межі — так */
  SmartLock_SecurityInit();
  pir(0);
  (void)poll(20000U);
  pir(1);
  (void)poll(21000U);
  pir(0);
  (void)poll(21020U);
  pir(1);
  (void)poll(21000U + TBD_PIR_MIN_INTERVAL_MS);
  drain();
  TEST_ASSERT_EQUAL_UINT(2U, cnt[SL_EV_MOTION]);
}

static void test_pir_edge_flag_is_single_event(void)
{
  door_open();
  (void)poll(0U);
  (void)poll(TBD_DOOR_DEBOUNCE_MS);
  drain();

  /* фронт з EXTI і рівень в одному проході — одна подія */
  pir(1);
  fake_platform.now_ms = 1000U;
  TEST_ASSERT_TRUE(SmartLock_SecurityPoll(1000U, true, false));
  expect_ev(SL_EV_MOTION, 0U);
  expect_none();
  (void)poll(1020U);   /* рівень ще високий: фронту немає */
  expect_none();

  /* EXTI спіймав короткий імпульс, а на момент опитування рівень уже 0 */
  pir(0);
  TEST_ASSERT_TRUE(SmartLock_SecurityPoll(20000U, true, false));
  expect_ev(SL_EV_MOTION, 0U);

  /* фронт у проході k, рівень піднявся лише в k+1 — друга подія гаситься інтервалом */
  pir(0);
  TEST_ASSERT_TRUE(SmartLock_SecurityPoll(40000U, true, false));
  pir(1);
  (void)poll(40020U);
  drain();
  TEST_ASSERT_EQUAL_UINT(1U, cnt[SL_EV_MOTION]);

  /* перший виклик після старту: pir_edge теж фронт */
  SmartLock_SecurityInit();
  pir(0);
  TEST_ASSERT_TRUE(SmartLock_SecurityPoll(50000U, true, true));
  drain();
  TEST_ASSERT_EQUAL_UINT(1U, cnt[SL_EV_MOTION]);
}

static void test_pir_queue_full_counts_lost_and_retries_next_pulse(void)
{
  door_open();
  (void)poll(0U);
  (void)poll(TBD_DOOR_DEBOUNCE_MS);
  drain();

  fill_queue();
  pir(1);
  TEST_ASSERT_FALSE(poll(1000U));
  TEST_ASSERT_EQUAL_UINT32(1U, SlSecurity_EventsLost());
  drain();
  pir(0);
  (void)poll(1020U);
  pir(1);
  (void)poll(1040U);   /* мітка часу не оновлювалась — наступний імпульс проходить */
  drain();
  TEST_ASSERT_EQUAL_UINT(1U, cnt[SL_EV_MOTION]);
  TEST_ASSERT_EQUAL_UINT32(1U, SlSecurity_EventsLost());
}

/* ---- MPU-6050: спокій і рух ------------------------------------------------------------- */
static void test_mpu_quiet_noise_log09_gives_no_tamper(void)
{
  uint32_t t = start_mpu_at(0U, NOISE_L09_MG[0][0], NOISE_L09_MG[0][1], NOISE_L09_MG[0][2]);

  feed(NOISE_L09_MG, (unsigned)(sizeof(NOISE_L09_MG) / sizeof(NOISE_L09_MG[0])), &t);
  expect_none();
  TEST_ASSERT_TRUE(SlSecurity_MpuHealthy());
  TEST_ASSERT_EQUAL_UINT32(0U, SlSecurity_MpuErrors());
  TEST_ASSERT_EQUAL_UINT32(0U, SlSecurity_MpuZeroSamples());
}

static void test_mpu_quiet_noise_log11_gives_no_tamper(void)
{
  uint32_t t = start_mpu_at(0U, NOISE_L11_MG[0][0], NOISE_L11_MG[0][1], NOISE_L11_MG[0][2]);

  feed(NOISE_L11_MG, (unsigned)(sizeof(NOISE_L11_MG) / sizeof(NOISE_L11_MG[0])), &t);
  expect_none();
  TEST_ASSERT_TRUE(SlSecurity_MpuHealthy());
}

static void test_mpu_rotation_from_log11_gives_no_tamper(void)
{
  const unsigned pts = (unsigned)(sizeof(ROTATION_L11_MG) / sizeof(ROTATION_L11_MG[0]));
  const int      steps = 55;   /* 1,1 с між зразками логу / 20 мс */
  uint32_t       t = start_mpu_at(0U, ROTATION_L11_MG[0][0], ROTATION_L11_MG[0][1], ROTATION_L11_MG[0][2]);

  for (unsigned p = 0U; (p + 1U) < pts; p++)
  {
    for (int k = 1; k <= steps; k++)
    {
      int x = ROTATION_L11_MG[p][0] + (((ROTATION_L11_MG[p + 1U][0] - ROTATION_L11_MG[p][0]) * k) / steps);
      int y = ROTATION_L11_MG[p][1] + (((ROTATION_L11_MG[p + 1U][1] - ROTATION_L11_MG[p][1]) * k) / steps);
      int z = ROTATION_L11_MG[p][2] + (((ROTATION_L11_MG[p + 1U][2] - ROTATION_L11_MG[p][2]) * k) / steps);

      accel_mg(x, y, z);
      (void)poll(t);
      t += STEP_MS;
    }
  }
  drain();
  TEST_ASSERT_EQUAL_UINT(0U, cnt[SL_EV_TAMPER]);
}

static void test_mpu_jump_gives_single_tamper_with_excess_arg(void)
{
  uint32_t t = start_mpu();

  /* +0,5 g по Z: сума |Δ| = 500 мg, понад поріг на 500 - TBD_TAMPER_THRESHOLD_MG */
  accel_mg(0, 0, 1500);
  TEST_ASSERT_TRUE(poll(t));
  expect_ev(SL_EV_TAMPER, 500U - TBD_TAMPER_THRESHOLD_MG);
  expect_none();

  /* повернення в початкове положення — той самий стрибок, але в межах інтервалу */
  t += STEP_MS;
  accel_mg(0, 0, 1000);
  (void)poll(t);
  expect_none();
  t += STEP_MS;
  run(&t, 10U);
  expect_none();
}

static void test_mpu_metric_sums_axes_and_threshold_is_strict(void)
{
  uint32_t t = start_mpu();

  /* рівно поріг (3277 LSB = 200,01 мg → 200): не понад поріг */
  fake_sensors_mpu_set_accel(0, 0, (int16_t)(MPU6050_ACCEL_LSB_PER_G + 3277));
  (void)poll(t);
  t += STEP_MS;
  fake_sensors_mpu_set_accel(0, 0, MPU6050_ACCEL_LSB_PER_G);
  (void)poll(t);
  t += STEP_MS;
  expect_none();

  /* 3294 LSB = 201,04 мg → arg 1 */
  fake_sensors_mpu_set_accel(0, 0, (int16_t)(MPU6050_ACCEL_LSB_PER_G + 3294));
  (void)poll(t);
  expect_ev(SL_EV_TAMPER, 1U);

  /* кілька осей разом (інтервал минув): відносно попереднього зразка
   * |4096| + |-4096| + |3294 - 2048| = 9438 LSB = 576 мg */
  t += TBD_TAMPER_MIN_INTERVAL_MS;
  fake_sensors_mpu_set_accel(4096, -4096, (int16_t)(MPU6050_ACCEL_LSB_PER_G + 2048));
  (void)poll(t);
  expect_ev(SL_EV_TAMPER, 576U - TBD_TAMPER_THRESHOLD_MG);
}

static void test_mpu_full_scale_swing_does_not_overflow(void)
{
  uint32_t t = start_mpu();

  fake_sensors_mpu_set_accel(-32768, 32767, -32768);   /* Δ = 32768 + 32767 + 49152 LSB */
  (void)poll(t);
  expect_ev(SL_EV_TAMPER, ((32768U + 32767U + 49152U) * 125U >> 11) - TBD_TAMPER_THRESHOLD_MG);
}

static void test_mpu_tamper_repeat_interval(void)
{
  uint32_t t = start_mpu();
  uint32_t t0 = t;

  accel_mg(0, 0, 1500);
  (void)poll(t0);
  expect_ev(SL_EV_TAMPER, 300U);

  accel_mg(0, 0, 1000);   /* повернення — стрибок гаситься */
  (void)poll(t0 + STEP_MS);
  accel_mg(0, 0, 1500);
  (void)poll(t0 + TBD_TAMPER_MIN_INTERVAL_MS - 1U);   /* ще в інтервалі */
  expect_none();
  accel_mg(0, 0, 1000);
  (void)poll(t0 + TBD_TAMPER_MIN_INTERVAL_MS);   /* інтервал минув: нове відхилення — подія */
  expect_ev(SL_EV_TAMPER, 300U);
  expect_none();
}

static void test_mpu_baseline_samples_give_no_events(void)
{
  uint32_t t = 0U;

  TEST_ASSERT_EQUAL(DEV_OK, fake_sensors_mpu_attach());
  door_open();
  /* під час встановлення бази датчик різко змінюється кожен зразок */
  for (unsigned i = 0U; i < TBD_MPU_BASELINE_SAMPLES; i++)
  {
    accel_mg(0, 0, ((i % 2U) == 0U) ? 1000 : 1500);
    TEST_ASSERT_TRUE(poll(t));
    t += STEP_MS;
  }
  drain();
  TEST_ASSERT_EQUAL_UINT(0U, cnt[SL_EV_TAMPER]);

  accel_mg(0, 0, 1500);   /* після бази порівняння працює */
  (void)poll(t);
  drain();
  TEST_ASSERT_EQUAL_UINT(1U, cnt[SL_EV_TAMPER]);
}

static void test_mpu_one_transaction_per_poll_and_edge_flag_ignored(void)
{
  uint32_t t = start_mpu();
  size_t   n0 = fake_i2c_log_count();

  (void)poll(t);
  TEST_ASSERT_EQUAL_UINT(n0 + 1U, fake_i2c_log_count());
  n0 = fake_i2c_log_count();
  TEST_ASSERT_TRUE(SmartLock_SecurityPoll(t + STEP_MS, false, true));   /* mpu_edge: без наслідків */
  TEST_ASSERT_EQUAL_UINT(n0 + 1U, fake_i2c_log_count());
  expect_none();
}

static void test_motion_and_tamper_in_one_poll_are_two_events(void)
{
  uint32_t t = start_mpu();

  pir(1);
  accel_mg(0, 0, 1500);
  TEST_ASSERT_TRUE(poll(t));
  TEST_ASSERT_EQUAL_UINT(2U, SmartLock_EventsPending());
  expect_ev(SL_EV_MOTION, 0U);
  expect_ev(SL_EV_TAMPER, 300U);
  expect_none();
}

static void test_all_three_in_one_poll_keep_order(void)
{
  uint32_t t = start_mpu();

  door_closed();
  for (unsigned i = 0U; i < (TBD_DOOR_DEBOUNCE_MS / STEP_MS); i++)
  {
    (void)poll(t + (i * STEP_MS));   /* антидребезг ще триває */
  }
  expect_none();
  /* опитування рівно в момент кінця антидребезгу: двері, рух, удар в одному проході */
  pir(1);
  accel_mg(0, 0, 1500);
  TEST_ASSERT_TRUE(poll(t + TBD_DOOR_DEBOUNCE_MS));
  expect_ev(SL_EV_DOOR_CLOSED, 0U);
  expect_ev(SL_EV_MOTION, 0U);
  expect_ev(SL_EV_TAMPER, 300U);
  expect_none();
}

static void test_tamper_queue_full_counts_lost(void)
{
  uint32_t t = start_mpu();

  fill_queue();
  accel_mg(0, 0, 1500);
  (void)poll(t);
  TEST_ASSERT_EQUAL_UINT32(1U, SlSecurity_EventsLost());
  drain();
  TEST_ASSERT_EQUAL_UINT(0U, cnt[SL_EV_TAMPER]);

  accel_mg(0, 0, 1000);   /* мітку часу не оновлено: наступне відхилення проходить одразу */
  (void)poll(t + STEP_MS);
  drain();
  TEST_ASSERT_EQUAL_UINT(1U, cnt[SL_EV_TAMPER]);
}

/* ---- MPU-6050: відмови ------------------------------------------------------------------ */
static void test_mpu_status_not_ok_is_never_read(void)
{
  uint32_t t = 0U;

  TEST_ASSERT_FALSE(SlSecurity_MpuHealthy());
  app_devices.mpu_status = DEV_ERR_STATE;
  TEST_ASSERT_FALSE(poll(t));   /* нічого корисного: ні подій, ні читання */
  TEST_ASSERT_EQUAL_UINT(0U, fake_i2c_log_count());
  TEST_ASSERT_FALSE(SlSecurity_MpuHealthy());

  /* геркон і PIR працюють без MPU */
  door_closed();
  pir(0);
  (void)poll(20U);
  (void)poll(20U + TBD_DOOR_DEBOUNCE_MS);
  expect_ev(SL_EV_DOOR_CLOSED, 0U);
  pir(1);
  TEST_ASSERT_TRUE(poll(5000U));
  expect_ev(SL_EV_MOTION, 0U);
  TEST_ASSERT_EQUAL_UINT(0U, fake_i2c_log_count());
  TEST_ASSERT_EQUAL_UINT32(0U, SlSecurity_MpuErrors());

  /* після повторної ініціалізації (статус знову OK) база збирається заново */
  TEST_ASSERT_EQUAL(DEV_OK, fake_sensors_mpu_attach());
  TEST_ASSERT_TRUE(SlSecurity_MpuHealthy());
  t = 6000U;
  accel_mg(0, 0, 1500);
  run(&t, TBD_MPU_BASELINE_SAMPLES);
  drain();
  TEST_ASSERT_EQUAL_UINT(0U, cnt[SL_EV_TAMPER]);
}

static void test_mpu_status_lost_midway_resets_baseline(void)
{
  uint32_t t = start_mpu();

  app_devices.mpu_status = DEV_ERR_STATE;
  (void)poll(t);
  TEST_ASSERT_FALSE(SlSecurity_MpuHealthy());
  app_devices.mpu_status = DEV_OK;
  accel_mg(0, 0, 1500);   /* зовсім інше положення, але це вже нова база */
  t += STEP_MS;
  run(&t, TBD_MPU_BASELINE_SAMPLES);
  drain();
  TEST_ASSERT_EQUAL_UINT(0U, cnt[SL_EV_TAMPER]);
  TEST_ASSERT_TRUE(SlSecurity_MpuHealthy());
}

static void test_mpu_single_i2c_error_is_counted_without_event(void)
{
  uint32_t t = start_mpu();

  fake_i2c_fail_at(fake_i2c_log_count(), DEV_ERR_TIMEOUT);
  TEST_ASSERT_TRUE(poll(t));   /* транзакція була, хоч і невдала */
  t += STEP_MS;
  TEST_ASSERT_EQUAL_UINT32(1U, SlSecurity_MpuErrors());
  TEST_ASSERT_TRUE(SlSecurity_MpuHealthy());
  expect_none();

  accel_mg(0, 0, 1500);   /* база збережена: удар одразу після збою помітний */
  (void)poll(t);
  expect_ev(SL_EV_TAMPER, 300U);
  TEST_ASSERT_EQUAL_UINT32(1U, SlSecurity_MpuErrors());
}

static void test_mpu_i2c_error_series_marks_fault_backs_off_and_recovers(void)
{
  uint32_t t = start_mpu();
  uint32_t t_fault = 0U;
  size_t   n;

  /* модуль «зник» із шини (logs/10, 15): кожне читання — помилка */
  fake_i2c_reset();
  for (unsigned i = 0U; i < TBD_MPU_ERR_LIMIT; i++)
  {
    TEST_ASSERT_TRUE(poll(t));
    t_fault = t;
    t += STEP_MS;
    TEST_ASSERT_EQUAL_UINT32(i + 1U, SlSecurity_MpuErrors());
  }
  expect_none();
  TEST_ASSERT_FALSE(SlSecurity_MpuHealthy());

  /* шина мовчить: до кінця паузи нових транзакцій немає */
  n = fake_i2c_log_count();
  for (; t < (t_fault + TBD_MPU_RETRY_MS); t += STEP_MS)
  {
    TEST_ASSERT_FALSE(poll(t));
  }
  TEST_ASSERT_EQUAL_UINT(n, fake_i2c_log_count());
  TEST_ASSERT_EQUAL_UINT32(TBD_MPU_ERR_LIMIT, SlSecurity_MpuErrors());

  /* пауза минула — одна спроба, знову помилка */
  t = t_fault + TBD_MPU_RETRY_MS;
  TEST_ASSERT_TRUE(poll(t));
  TEST_ASSERT_EQUAL_UINT(n + 1U, fake_i2c_log_count());
  TEST_ASSERT_EQUAL_UINT32(TBD_MPU_ERR_LIMIT + 1U, SlSecurity_MpuErrors());
  TEST_ASSERT_FALSE(SlSecurity_MpuHealthy());

  /* модуль повернувся в іншому положенні: це нова база, не TAMPER */
  TEST_ASSERT_EQUAL(DEV_OK, fake_sensors_mpu_attach());
  accel_mg(500, 0, 866);
  t += TBD_MPU_RETRY_MS;
  TEST_ASSERT_TRUE(poll(t));
  TEST_ASSERT_TRUE(SlSecurity_MpuHealthy());
  t += STEP_MS;
  run(&t, TBD_MPU_BASELINE_SAMPLES - 1U);
  expect_none();

  accel_mg(500, 0, 1866);   /* після бази TAMPER знову працює */
  (void)poll(t);
  expect_ev(SL_EV_TAMPER, 1000U - TBD_TAMPER_THRESHOLD_MG);
}

static void test_mpu_fault_does_not_affect_door_and_pir(void)
{
  uint32_t t = 0U;

  TEST_ASSERT_EQUAL(DEV_OK, fake_sensors_mpu_attach());
  fake_i2c_reset();   /* MPU зник */
  door_closed();
  run(&t, 8U);
  expect_ev(SL_EV_DOOR_CLOSED, 0U);
  pir(1);
  (void)poll(t);
  expect_ev(SL_EV_MOTION, 0U);
  TEST_ASSERT_FALSE(SlSecurity_MpuHealthy());
  door_open();
  t += STEP_MS;
  (void)poll(t);
  (void)poll(t + TBD_DOOR_DEBOUNCE_MS);
  expect_ev(SL_EV_DOOR_OPEN, 0U);
}

static void test_mpu_all_zero_axes_mark_fault_without_tamper(void)
{
  uint32_t t = start_mpu();

  fake_sensors_mpu_set_accel(0, 0, 0);   /* модуль без живлення відповідає нулями (лог 08, 15) */
  for (unsigned i = 0U; i < TBD_MPU_ZERO_LIMIT - 1U; i++)
  {
    TEST_ASSERT_TRUE(poll(t));
    t += STEP_MS;
    TEST_ASSERT_TRUE(SlSecurity_MpuHealthy());   /* поодинокі нулі ще не відмова */
  }
  TEST_ASSERT_TRUE(poll(t));
  t += STEP_MS;
  TEST_ASSERT_FALSE(SlSecurity_MpuHealthy());
  TEST_ASSERT_EQUAL_UINT32(TBD_MPU_ZERO_LIMIT, SlSecurity_MpuZeroSamples());
  run(&t, 20U);
  expect_none();   /* стрибок 1 g → 0 не є ударом */
  TEST_ASSERT_EQUAL_UINT32(TBD_MPU_ZERO_LIMIT + 20U, SlSecurity_MpuZeroSamples());
  TEST_ASSERT_EQUAL_UINT32(0U, SlSecurity_MpuErrors());

  /* живлення повернулось: 0 → 1 g — не TAMPER, база збирається заново */
  accel_mg(0, 0, 1000);
  run(&t, TBD_MPU_BASELINE_SAMPLES);
  TEST_ASSERT_TRUE(SlSecurity_MpuHealthy());
  expect_none();

  accel_mg(0, 0, 1500);
  (void)poll(t);
  expect_ev(SL_EV_TAMPER, 300U);
}

static void test_mpu_isolated_zero_sample_is_skipped(void)
{
  uint32_t t = start_mpu();

  fake_sensors_mpu_set_accel(0, 0, 0);
  (void)poll(t);
  t += STEP_MS;
  accel_mg(0, 0, 1000);
  (void)poll(t);
  t += STEP_MS;
  expect_none();   /* 1 g → 0 → 1 g без TAMPER */
  TEST_ASSERT_TRUE(SlSecurity_MpuHealthy());
  TEST_ASSERT_EQUAL_UINT32(1U, SlSecurity_MpuZeroSamples());

  accel_mg(0, 0, 1500);   /* база збережена: удар помітний */
  (void)poll(t);
  expect_ev(SL_EV_TAMPER, 300U);
}

static void test_mpu_zero_accel_with_nonzero_gyro_is_not_a_fault(void)
{
  uint32_t t = start_mpu();
  uint8_t *r = fake_i2c_device_regs(MPU6050_I2C_ADDR);

  r[MPU6050_REG_ACCEL_XOUT_H + 9U] = 0x01U;   /* gyro X ≠ 0 */
  fake_sensors_mpu_set_accel(0, 0, 0);          /* вільне падіння: 1 g → 0 */
  for (unsigned i = 0U; i < TBD_MPU_ZERO_LIMIT + 1U; i++)
  {
    (void)poll(t);
    t += STEP_MS;
  }
  TEST_ASSERT_TRUE(SlSecurity_MpuHealthy());
  TEST_ASSERT_EQUAL_UINT32(0U, SlSecurity_MpuZeroSamples());
  drain();
  TEST_ASSERT_EQUAL_UINT(1U, cnt[SL_EV_TAMPER]);   /* стрибок 1 g → 0 — це удар / падіння */
}

/* ---- переповнення Platform_Millis() ---------------------------------------------------- */
static void test_millis_wrap_door_debounce(void)
{
  const uint32_t t0 = 0xFFFFFFC0U;

  door_open();
  (void)poll(t0);
  (void)poll(t0 + TBD_DOOR_DEBOUNCE_MS - 1U);
  expect_none();
  (void)poll(t0 + TBD_DOOR_DEBOUNCE_MS);   /* уже після переповнення */
  expect_ev(SL_EV_DOOR_OPEN, 0U);

  door_closed();
  (void)poll(0x00001000U);
  (void)poll(0x00001000U + TBD_DOOR_DEBOUNCE_MS - 1U);
  expect_none();
  (void)poll(0x00001000U + TBD_DOOR_DEBOUNCE_MS);
  expect_ev(SL_EV_DOOR_CLOSED, 0U);
}

static void test_millis_wrap_pir_interval(void)
{
  const uint32_t t0 = 0xFFFFFFF0U;

  door_open();
  (void)poll(t0 - 2000U);
  pir(1);
  (void)poll(t0);
  expect_ev(SL_EV_DOOR_OPEN, 0U);   /* початковий стан дверей підтверджено в цьому ж проході */
  expect_ev(SL_EV_MOTION, 0U);
  pir(0);
  (void)poll(t0 + 20U);
  pir(1);
  (void)poll(t0 + TBD_PIR_MIN_INTERVAL_MS - 1U);   /* wrap всередині інтервалу */
  expect_none();
  pir(0);
  (void)poll(t0 + TBD_PIR_MIN_INTERVAL_MS);
  pir(1);
  (void)poll(t0 + TBD_PIR_MIN_INTERVAL_MS + 20U);
  expect_ev(SL_EV_MOTION, 0U);
}

static void test_millis_wrap_tamper_interval(void)
{
  uint32_t t = start_mpu_at(0xFFFFFF00U, 0, 0, 1000);   /* 11 опитувань, час іде через 2^32 уже далі */

  t = 0xFFFFFFE0U;
  accel_mg(0, 0, 1500);
  (void)poll(t);
  expect_ev(SL_EV_TAMPER, 300U);
  accel_mg(0, 0, 1000);
  (void)poll(t + 20U);
  accel_mg(0, 0, 1500);
  (void)poll(t + TBD_TAMPER_MIN_INTERVAL_MS - 1U);   /* wrap усередині інтервалу */
  expect_none();
  accel_mg(0, 0, 1000);
  (void)poll(t + TBD_TAMPER_MIN_INTERVAL_MS);
  expect_ev(SL_EV_TAMPER, 300U);
}

int main(void)
{
  UNITY_BEGIN();
  RUN_TEST(test_init_posts_nothing);
  RUN_TEST(test_init_resets_door_state_and_counters);
  RUN_TEST(test_door_initial_closed_after_debounce);
  RUN_TEST(test_door_initial_open_after_debounce);
  RUN_TEST(test_door_chatter_before_first_state);
  RUN_TEST(test_door_bounce_and_short_pulses_give_no_events);
  RUN_TEST(test_door_open_and_close_events);
  RUN_TEST(test_door_queue_full_retries_until_posted);
  RUN_TEST(test_door_pending_keeps_order_of_new_transition);
  RUN_TEST(test_door_pending_then_state_returns);
  RUN_TEST(test_pir_rising_edge_gives_motion);
  RUN_TEST(test_pir_held_level_and_falling_edge_give_nothing);
  RUN_TEST(test_pir_high_at_start_is_baseline_only);
  RUN_TEST(test_pir_pulse_series_merged_by_min_interval);
  RUN_TEST(test_pir_min_interval_boundary);
  RUN_TEST(test_pir_edge_flag_is_single_event);
  RUN_TEST(test_pir_queue_full_counts_lost_and_retries_next_pulse);
  RUN_TEST(test_mpu_quiet_noise_log09_gives_no_tamper);
  RUN_TEST(test_mpu_quiet_noise_log11_gives_no_tamper);
  RUN_TEST(test_mpu_rotation_from_log11_gives_no_tamper);
  RUN_TEST(test_mpu_jump_gives_single_tamper_with_excess_arg);
  RUN_TEST(test_mpu_metric_sums_axes_and_threshold_is_strict);
  RUN_TEST(test_mpu_full_scale_swing_does_not_overflow);
  RUN_TEST(test_mpu_tamper_repeat_interval);
  RUN_TEST(test_mpu_baseline_samples_give_no_events);
  RUN_TEST(test_mpu_one_transaction_per_poll_and_edge_flag_ignored);
  RUN_TEST(test_motion_and_tamper_in_one_poll_are_two_events);
  RUN_TEST(test_all_three_in_one_poll_keep_order);
  RUN_TEST(test_tamper_queue_full_counts_lost);
  RUN_TEST(test_mpu_status_not_ok_is_never_read);
  RUN_TEST(test_mpu_status_lost_midway_resets_baseline);
  RUN_TEST(test_mpu_single_i2c_error_is_counted_without_event);
  RUN_TEST(test_mpu_i2c_error_series_marks_fault_backs_off_and_recovers);
  RUN_TEST(test_mpu_fault_does_not_affect_door_and_pir);
  RUN_TEST(test_mpu_all_zero_axes_mark_fault_without_tamper);
  RUN_TEST(test_mpu_isolated_zero_sample_is_skipped);
  RUN_TEST(test_mpu_zero_accel_with_nonzero_gyro_is_not_a_fault);
  RUN_TEST(test_millis_wrap_door_debounce);
  RUN_TEST(test_millis_wrap_pir_interval);
  RUN_TEST(test_millis_wrap_tamper_interval);
  return UNITY_END();
}
