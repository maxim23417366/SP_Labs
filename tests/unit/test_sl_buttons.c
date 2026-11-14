/**
  * @file    test_sl_buttons.c
  * @brief   ЛР3 (K03/K04): USER B1 і код з 4 кнопок — sl_buttons.c без плати.
  *
  *          ЗАГОТОВКА для власника картки. Збирається з усім App/ і
  *          fake_platform (tests/Makefile, APP_TESTS). Окремий запуск:
  *            make -C tests run-sl_buttons
  *
  *          Залізо: fake_platform (входи PLATFORM_IN_*, now_ms, servo_log,
  *          dht_script), fake_sensors (MPU-6050 / BMP180 через app_devices),
  *          fake_i2c (помилки шини). Події перевіряти через SmartLock_TakeEvent().
  */
#include <string.h>

#include "unity.h"

#include "app.h"
#include "event_queue.h"
#include "fake_i2c.h"
#include "fake_platform.h"
#include "fake_sensors.h"
#include "sl_buttons.h"

void setUp(void)
{
  fake_platform_reset();
  fake_i2c_reset();
  memset(&app_devices, 0, sizeof(app_devices));
  app_devices.mpu_status  = DEV_ERR_STATE;
  app_devices.baro_status = DEV_ERR_STATE;
  SmartLock_EventsInit();
  SmartLock_ButtonsInit();
}

void tearDown(void)
{
}

static void test_init_posts_nothing(void)
{
  SmartLock_ButtonsInit();
  TEST_ASSERT_EQUAL_UINT(0U, SmartLock_EventsPending());
}

int main(void)
{
  UNITY_BEGIN();
  RUN_TEST(test_init_posts_nothing);
  return UNITY_END();
}
