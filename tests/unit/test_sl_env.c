/**
  * @file    test_sl_env.c
  * @brief   ЛР3 (K06): DHT11 / BMP180, ENV_WARNING — sl_env.c без плати.
  *
  *          ЗАГОТОВКА для власника картки. Збирається з усім App/ і
  *          fake_platform (tests/Makefile, APP_TESTS). Окремий запуск:
  *            make -C tests run-sl_env
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
#include "sl_env.h"

void setUp(void)
{
  fake_platform_reset();
  fake_i2c_reset();
  memset(&app_devices, 0, sizeof(app_devices));
  app_devices.mpu_status  = DEV_ERR_STATE;
  app_devices.baro_status = DEV_ERR_STATE;
  SmartLock_EventsInit();
  SmartLock_EnvInit();
}

void tearDown(void)
{
}

static void test_init_posts_nothing(void)
{
  SmartLock_EnvInit();
  TEST_ASSERT_EQUAL_UINT(0U, SmartLock_EventsPending());
}

int main(void)
{
  UNITY_BEGIN();
  RUN_TEST(test_init_posts_nothing);
  return UNITY_END();
}
