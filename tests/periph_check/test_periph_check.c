/**
  * @file    test_periph_check.c
  * @brief   Тести periph_check.c на ПК з імітованим HAL (без плати).
  *
  *          Кожен сценарій налаштовує «залізо» у mock, запускає ту саму
  *          функцію, що й прошивка, і перевіряє висновки модуля. Лог прошивки
  *          друкується як є — так він виглядатиме в UART на реальній платі.
  *
  *          Що НЕ перевіряється: реальні сигнали на шинах, поведінка
  *          справжнього HAL і датчиків — це робиться на платі аналізатором.
  */
#include <stdio.h>
#include <string.h>

#include "main.h"
#include "periph_check.h"

static unsigned checks_total;
static unsigned checks_failed;

#define CHECK(cond)  check_impl((cond) ? 1 : 0, #cond, __LINE__)

static void check_impl(int ok, const char *expr, int line)
{
  checks_total++;
  if (!ok)
  {
    checks_failed++;
    printf("  !! CHECK FAILED (line %d): %s\n", line, expr);
  }
}

/* Час іде лише вперед: PeriphCheck_Loop() пам'ятає таймстемпи між викликами */
static void run_loop_for(uint32_t duration_ms, uint32_t step_ms)
{
  for (uint32_t t = 0U; t < duration_ms; t += step_ms)
  {
    mock.tick_ms += step_ms;
    PeriphCheck_Loop();
  }
}

static void run_check(void)
{
  memset(&periph_check, 0, sizeof(periph_check));
  PeriphCheck_RunAll();
}

/* ------------------------------------------------------------------------- */

static void test_all_connected(void)
{
  static const uint8_t expected[] = { 0x1DU, 0x21U, 0x68U, 0x6BU, 0x77U };

  run_check();

  CHECK(periph_check.init_ok);
  CHECK(periph_check.clocks_ok);
  CHECK(periph_check.sysclk_hz == 96000000UL);
  CHECK(periph_check.pclk1_hz == 48000000UL);

  CHECK(periph_check.i2c_ok);
  CHECK(!periph_check.i2c_bus_busy);
  CHECK(periph_check.mpu6050_present);
  CHECK(periph_check.ov7670_present);
  CHECK(periph_check.i2c_found_count == sizeof(expected));
  CHECK(memcmp(periph_check.i2c_found, expected, sizeof(expected)) == 0);
  /* скан рівно 0x08..0x77, без зарезервованих адрес */
  CHECK(mock.i2c_min_addr == 0x08U);
  CHECK(mock.i2c_max_addr == 0x77U);
  CHECK(mock.i2c_probe_count == 0x70U);

  CHECK(periph_check.flash_ok);
  CHECK(periph_check.jedec_id[0] == 0xEFU);
  CHECK(periph_check.jedec_id[1] == 0x40U);
  CHECK(periph_check.jedec_id[2] == 0x17U);
  CHECK(mock.flash_transfers == 1U);
  CHECK(mock.flash_protocol_errors == 0U);  /* CS опущений, команда 0x9F */
  CHECK(mock.flash_cs_level == 1U);         /* CS відпущений після обміну */

  CHECK(periph_check.pwm_ok);
  CHECK(periph_check.pwm_freq_hz == 50U);
  CHECK(periph_check.pwm_pulse_us == 1500U);
  CHECK((mock_tim3.CR1 & TIM_CR1_CEN) != 0U);

  CHECK(periph_check.all_ok);
}

static void test_no_modules_bus_busy(void)
{
  /* Модулі не підключені: немає підтяжок на I2C, MISO «висить» у 1 */
  mock.i2c_bus_busy = 1U;
  memset(mock.jedec_id, 0xFF, sizeof(mock.jedec_id));

  run_check();

  CHECK(periph_check.i2c_bus_busy);
  CHECK(mock.i2c_probe_count == 1U);  /* скан зупиняється одразу */
  CHECK(periph_check.i2c_found_count == 0U);
  CHECK(!periph_check.i2c_ok);
  CHECK(!periph_check.flash_ok);
  /* ініціалізація, тактування і PWM від датчиків не залежать */
  CHECK(periph_check.init_ok);
  CHECK(periph_check.clocks_ok);
  CHECK(periph_check.pwm_ok);
  CHECK(!periph_check.all_ok);
}

static void test_camera_without_xclk(void)
{
  /* Без XCLK 24 МГц на PA8 OV7670 не відповідає по SCCB */
  mock.i2c_ack[0x21] = 0U;

  run_check();

  CHECK(!periph_check.ov7670_present);
  CHECK(periph_check.mpu6050_present);
  CHECK(periph_check.i2c_found_count == 4U);
  CHECK(!periph_check.i2c_ok);
  CHECK(periph_check.flash_ok);
  CHECK(!periph_check.all_ok);
}

static void test_mpu6050_missing(void)
{
  mock.i2c_ack[0x68] = 0U;

  run_check();

  CHECK(!periph_check.mpu6050_present);
  CHECK(periph_check.ov7670_present);
  CHECK(!periph_check.i2c_ok);
  CHECK(!periph_check.all_ok);
}

static void test_flash_reads_ff(void)
{
  memset(mock.jedec_id, 0xFF, sizeof(mock.jedec_id));

  run_check();

  CHECK(!periph_check.flash_ok);
  CHECK(periph_check.jedec_id[0] == 0xFFU);
  CHECK(mock.flash_cs_level == 1U);
  CHECK(periph_check.i2c_ok);
  CHECK(!periph_check.all_ok);
}

static void test_flash_reads_00(void)
{
  memset(mock.jedec_id, 0x00, sizeof(mock.jedec_id));

  run_check();

  CHECK(!periph_check.flash_ok);
  CHECK(!periph_check.all_ok);
}

static void test_spi_transfer_error(void)
{
  mock.spi_status = HAL_TIMEOUT;

  run_check();

  CHECK(!periph_check.flash_ok);
  CHECK(mock.flash_cs_level == 1U);  /* CS відпускається навіть після помилки */
  CHECK(!periph_check.all_ok);
}

static void test_wrong_clock_84mhz(void)
{
  /* Конфігурація за замовчуванням Nucleo (PLL від HSI): 84 / 84 / 42 / 84 МГц */
  mock.sysclk_hz = 84000000UL;
  mock.hclk_hz   = 84000000UL;
  mock.pclk1_hz  = 42000000UL;
  mock.pclk2_hz  = 84000000UL;

  run_check();

  CHECK(!periph_check.clocks_ok);
  /* PWM рахується з регістрів: 84 МГц / 96 / 20000 = 43 Гц, імпульс 1714 мкс */
  CHECK(periph_check.pwm_freq_hz == 43U);
  CHECK(periph_check.pwm_pulse_us == 1714U);
  CHECK(!periph_check.pwm_ok);
  CHECK(!periph_check.all_ok);
}

static void test_spi_handle_not_ready(void)
{
  hspi2.State = HAL_SPI_STATE_RESET;

  run_check();

  CHECK(!periph_check.init_ok);
  CHECK(!periph_check.all_ok);
}

static void test_pwm_start_error(void)
{
  mock.pwm_start_status = HAL_ERROR;

  run_check();

  CHECK(!periph_check.pwm_ok);
  CHECK((mock_tim3.CR1 & TIM_CR1_CEN) == 0U);
  CHECK(!periph_check.all_ok);
}

static void test_loop_hot_unplug_and_led(void)
{
  mock.tick_ms = 10000U;
  run_check();
  CHECK(periph_check.all_ok);

  /* Усе OK: LD2 перемикається кожні 500 мс -> 2 рази за секунду,
   * опитування шин теж кожні 500 мс */
  PeriphCheck_Loop();
  mock.led_toggles = 0U;
  uint32_t loops_before = periph_check.loop_count;
  uint32_t transfers_before = mock.flash_transfers;
  run_loop_for(1000U, 10U);
  CHECK(mock.led_toggles == 2U);
  CHECK(periph_check.loop_count - loops_before == 2U);
  CHECK(mock.flash_transfers - transfers_before == 2U);
  CHECK(mock.flash_protocol_errors == 0U);
  CHECK(periph_check.all_ok);

  /* MPU-6050 від'єднали на ходу */
  printf("  (unplug MPU-6050)\n");
  mock.i2c_ack[0x68] = 0U;
  run_loop_for(500U, 10U);
  CHECK(!periph_check.mpu6050_present);
  CHECK(!periph_check.i2c_ok);
  CHECK(!periph_check.all_ok);

  /* Помилка: LD2 перемикається кожні 100 мс -> 10 разів за секунду */
  mock.led_toggles = 0U;
  run_loop_for(1000U, 10U);
  CHECK(mock.led_toggles == 10U);

  /* Підключили назад: стан відновлюється без перезапуску */
  printf("  (plug MPU-6050 back)\n");
  mock.i2c_ack[0x68] = 1U;
  run_loop_for(500U, 10U);
  CHECK(periph_check.mpu6050_present);
  CHECK(periph_check.all_ok);
}

/* ------------------------------------------------------------------------- */

typedef struct
{
  const char *name;
  void (*fn)(void);
} TestCase;

static const TestCase tests[] =
{
  { "all devices connected",                 test_all_connected },
  { "no modules: I2C bus busy, MISO high",   test_no_modules_bus_busy },
  { "camera without XCLK (0x21 missing)",    test_camera_without_xclk },
  { "MPU-6050 missing (0x68)",               test_mpu6050_missing },
  { "flash not responding: FF FF FF",        test_flash_reads_ff },
  { "flash not responding: 00 00 00",        test_flash_reads_00 },
  { "SPI transfer timeout",                  test_spi_transfer_error },
  { "wrong clock tree: 84 MHz from HSI",     test_wrong_clock_84mhz },
  { "SPI2 handle not initialized",           test_spi_handle_not_ready },
  { "PWM start error",                       test_pwm_start_error },
  { "loop: hot unplug + LED blink rate",     test_loop_hot_unplug_and_led },
};

int main(void)
{
  const size_t count = sizeof(tests) / sizeof(tests[0]);
  unsigned result[sizeof(tests) / sizeof(tests[0])];

  for (size_t i = 0; i < count; i++)
  {
    printf("\n######## [%zu/%zu] %s ########", i + 1U, count, tests[i].name);
    unsigned failed_before = checks_failed;
    mock_reset();
    tests[i].fn();
    result[i] = checks_failed - failed_before;
  }

  printf("\n\n================ SUMMARY ================\n");
  for (size_t i = 0; i < count; i++)
  {
    printf("  %-4s %s\n", result[i] == 0U ? "PASS" : "FAIL", tests[i].name);
  }
  printf("-----------------------------------------\n");
  printf("  %u checks, %u failed -> %s\n", checks_total, checks_failed,
         checks_failed == 0U ? "ALL TESTS PASSED" : "TESTS FAILED");

  return checks_failed == 0U ? 0 : 1;
}
