/**
  * @file    test_bmp180.c
  * @brief   BMP180: приклад розрахунку з datasheet, калібрування, послідовність
  *          вимірювання і помилки шини.
  */
#include <string.h>

#include "unity.h"
#include "bmp180.h"
#include "fake_delay.h"
#include "fake_i2c.h"

/* Числовий приклад з datasheet BMP180 («Calculating pressure and temperature»):
 * oss = 0, UT = 27898, UP = 23843 -> T = 150 (15.0 °C), p = 69964 Па. */
static const bmp180_calib_t ds_calib =
{
  .ac1 = 408, .ac2 = -72, .ac3 = -14383, .ac4 = 32741, .ac5 = 32757, .ac6 = 23153,
  .b1 = 6190, .b2 = 4, .mb = -32768, .mc = -8711, .md = 2868,
};
#define DS_UT  27898
#define DS_UP  23843

/* Ті самі коефіцієнти так, як вони лежать в EEPROM 0xAA..0xBF (big-endian) */
static const uint8_t ds_calib_raw[BMP180_CALIB_LEN] =
{
  0x01, 0x98,  0xFF, 0xB8,  0xC7, 0xD1,  0x7F, 0xE5,  0x7F, 0xF5,  0x5A, 0x71,
  0x18, 0x2E,  0x00, 0x04,  0x80, 0x00,  0xDD, 0xF9,  0x0B, 0x34,
};

static uint8_t *regs;
static bmp180_t dev;

/* Імітація чипа: запис команди в 0xF4 кладе результат у 0xF6..0xF8 */
static void bmp180_chip_hook(uint8_t *r, uint8_t reg, uint8_t value)
{
  if (reg != BMP180_REG_CTRL_MEAS)
  {
    return;
  }
  if (value == BMP180_CMD_TEMP)
  {
    r[0xF6] = (uint8_t)(DS_UT >> 8);
    r[0xF7] = (uint8_t)DS_UT;
  }
  else
  {
    uint8_t  oss = (uint8_t)(value >> 6);
    uint32_t raw = (uint32_t)DS_UP << (8U - oss);
    r[0xF6] = (uint8_t)(raw >> 16);
    r[0xF7] = (uint8_t)(raw >> 8);
    r[0xF8] = (uint8_t)raw;
  }
}

void setUp(void)
{
  fake_i2c_reset();
  fake_delay_reset();
  regs = fake_i2c_add_device(BMP180_I2C_ADDR);
  regs[BMP180_REG_ID] = BMP180_CHIP_ID;
  memcpy(&regs[BMP180_REG_CALIB], ds_calib_raw, sizeof(ds_calib_raw));
  fake_i2c_set_write_hook(BMP180_I2C_ADDR, bmp180_chip_hook);
}

void tearDown(void)
{
}

static void test_datasheet_example_temperature(void)
{
  int32_t b5 = 0;

  TEST_ASSERT_EQUAL_INT32(150, bmp180_compensate_temp(&ds_calib, DS_UT, &b5));
  TEST_ASSERT_EQUAL_INT32(2400, b5);
}

static void test_datasheet_example_pressure(void)
{
  int32_t b5 = 0;

  (void)bmp180_compensate_temp(&ds_calib, DS_UT, &b5);
  TEST_ASSERT_EQUAL_INT32(69964, bmp180_compensate_pressure(&ds_calib, DS_UP, b5, 0U));
}

static void test_parse_calib_big_endian(void)
{
  bmp180_calib_t c;

  TEST_ASSERT_EQUAL_INT(1, bmp180_parse_calib(ds_calib_raw, &c));
  TEST_ASSERT_EQUAL_MEMORY(&ds_calib, &c, sizeof(c));
}

static void test_parse_calib_rejects_ffff_and_0000(void)
{
  uint8_t raw[BMP180_CALIB_LEN];
  bmp180_calib_t c;

  memcpy(raw, ds_calib_raw, sizeof(raw));
  raw[6] = 0xFF;  /* AC4 = 0xFFFF: EEPROM не прочиталась */
  raw[7] = 0xFF;
  TEST_ASSERT_EQUAL_INT(0, bmp180_parse_calib(raw, &c));

  memcpy(raw, ds_calib_raw, sizeof(raw));
  raw[20] = 0x00; /* MD = 0x0000 */
  raw[21] = 0x00;
  TEST_ASSERT_EQUAL_INT(0, bmp180_parse_calib(raw, &c));
}

static void test_init_reads_id_then_calibration(void)
{
  TEST_ASSERT_EQUAL(DEV_OK, bmp180_init(&dev, fake_i2c_bus(), fake_delay_ms, 0U));
  TEST_ASSERT_EQUAL_MEMORY(&ds_calib, &dev.calib, sizeof(ds_calib));

  TEST_ASSERT_EQUAL_size_t(2U, fake_i2c_log_count());
  TEST_ASSERT_EQUAL_HEX8(BMP180_REG_ID, fake_i2c_log(0)->wire[1]);
  TEST_ASSERT_EQUAL_HEX8(BMP180_REG_CALIB, fake_i2c_log(1)->wire[1]);
  /* 22 байти калібрування одним багатобайтовим читанням */
  TEST_ASSERT_EQUAL_size_t(3U + BMP180_CALIB_LEN, fake_i2c_log(1)->wire_len);
}

static void test_init_wrong_chip_id(void)
{
  regs[BMP180_REG_ID] = 0x58U;  /* BMP280 на схожому модулі */

  TEST_ASSERT_EQUAL(DEV_ERR_ID, bmp180_init(&dev, fake_i2c_bus(), fake_delay_ms, 0U));
}

static void test_init_device_absent_nack(void)
{
  fake_i2c_reset();

  TEST_ASSERT_EQUAL(DEV_ERR_NACK, bmp180_init(&dev, fake_i2c_bus(), fake_delay_ms, 0U));
}

static void test_init_bad_calibration(void)
{
  memset(&regs[BMP180_REG_CALIB], 0xFF, BMP180_CALIB_LEN);

  TEST_ASSERT_EQUAL(DEV_ERR_STATE, bmp180_init(&dev, fake_i2c_bus(), fake_delay_ms, 0U));
}

static void test_init_rejects_bad_params(void)
{
  TEST_ASSERT_EQUAL(DEV_ERR_PARAM, bmp180_init(&dev, fake_i2c_bus(), fake_delay_ms, 4U));
  TEST_ASSERT_EQUAL(DEV_ERR_PARAM, bmp180_init(&dev, NULL, fake_delay_ms, 0U));
  TEST_ASSERT_EQUAL(DEV_ERR_PARAM, bmp180_init(&dev, fake_i2c_bus(), NULL, 0U));
  TEST_ASSERT_EQUAL_size_t(0U, fake_i2c_log_count());
}

static void test_read_full_cycle_matches_datasheet(void)
{
  int32_t t = 0;
  int32_t p = 0;

  TEST_ASSERT_EQUAL(DEV_OK, bmp180_init(&dev, fake_i2c_bus(), fake_delay_ms, 0U));
  TEST_ASSERT_EQUAL(DEV_OK, bmp180_read(&dev, &t, &p));

  TEST_ASSERT_EQUAL_INT32(150, t);
  TEST_ASSERT_EQUAL_INT32(69964, p);
  /* 2 транзакції init + (запис 0xF4, читання) для T і для p */
  TEST_ASSERT_EQUAL_size_t(6U, fake_i2c_log_count());
  TEST_ASSERT_EQUAL_HEX8(BMP180_CMD_TEMP, fake_i2c_log(2)->wire[2]);
  TEST_ASSERT_EQUAL_HEX8(BMP180_CMD_PRESSURE, fake_i2c_log(4)->wire[2]);
  /* Час перетворення: 4.5 мс для T і для p при oss = 0 */
  TEST_ASSERT_EQUAL_UINT32(10U, fake_delay_total_ms);
}

static void test_read_oversampling_3(void)
{
  int32_t ut = 0;
  int32_t up = 0;

  TEST_ASSERT_EQUAL(DEV_OK, bmp180_init(&dev, fake_i2c_bus(), fake_delay_ms, 3U));
  TEST_ASSERT_EQUAL(DEV_OK, bmp180_read_raw(&dev, &ut, &up));

  TEST_ASSERT_EQUAL_INT32(DS_UT, ut);
  TEST_ASSERT_EQUAL_INT32(DS_UP, up);
  TEST_ASSERT_EQUAL_HEX8(0xF4U, fake_i2c_log(4)->wire[2]);  /* 0x34 | (3 << 6) */
  TEST_ASSERT_EQUAL_UINT32(5U + 26U, fake_delay_total_ms);
}

static void test_read_bus_errors_propagate(void)
{
  int32_t t = 0;
  int32_t p = 0;

  /* Збій на кожному з 4 кроків вимірювання: драйвер повертає помилку шини */
  for (size_t step = 0U; step < 4U; step++)
  {
    setUp();
    TEST_ASSERT_EQUAL(DEV_OK, bmp180_init(&dev, fake_i2c_bus(), fake_delay_ms, 0U));
    fake_i2c_fail_at(2U + step, DEV_ERR_TIMEOUT);
    TEST_ASSERT_EQUAL(DEV_ERR_TIMEOUT, bmp180_read(&dev, &t, &p));
    TEST_ASSERT_EQUAL_size_t(3U + step, fake_i2c_log_count());
  }
}

static void test_init_calibration_read_timeout(void)
{
  fake_i2c_fail_at(1U, DEV_ERR_TIMEOUT);

  TEST_ASSERT_EQUAL(DEV_ERR_TIMEOUT, bmp180_init(&dev, fake_i2c_bus(), fake_delay_ms, 0U));
}

int main(void)
{
  UNITY_BEGIN();
  RUN_TEST(test_datasheet_example_temperature);
  RUN_TEST(test_datasheet_example_pressure);
  RUN_TEST(test_parse_calib_big_endian);
  RUN_TEST(test_parse_calib_rejects_ffff_and_0000);
  RUN_TEST(test_init_reads_id_then_calibration);
  RUN_TEST(test_init_wrong_chip_id);
  RUN_TEST(test_init_device_absent_nack);
  RUN_TEST(test_init_bad_calibration);
  RUN_TEST(test_init_rejects_bad_params);
  RUN_TEST(test_read_full_cycle_matches_datasheet);
  RUN_TEST(test_read_oversampling_3);
  RUN_TEST(test_read_bus_errors_propagate);
  RUN_TEST(test_init_calibration_read_timeout);
  return UNITY_END();
}
