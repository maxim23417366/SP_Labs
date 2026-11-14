/**
  * @file    fake_sensors.c
  * @brief   MPU-6050 і BMP180 на fake_i2c для тестів App/ (див. fake_sensors.h).
  */
#include "fake_sensors.h"

#include <string.h>

#include "app.h"
#include "fake_i2c.h"
#include "platform.h"

/* Коефіцієнти прикладу datasheet BMP180 так, як вони лежать в EEPROM 0xAA..0xBF */
static const uint8_t bmp_ds_calib_raw[BMP180_CALIB_LEN] =
{
  0x01, 0x98,  0xFF, 0xB8,  0xC7, 0xD1,  0x7F, 0xE5,  0x7F, 0xF5,  0x5A, 0x71,
  0x18, 0x2E,  0x00, 0x04,  0x80, 0x00,  0xDD, 0xF9,  0x0B, 0x34,
};

static int32_t bmp_ut = FAKE_BMP180_DS_UT;
static int32_t bmp_up = FAKE_BMP180_DS_UP;

/* Імітація чипа: запис команди в 0xF4 кладе результат у 0xF6..0xF8 */
static void bmp180_chip_hook(uint8_t *r, uint8_t reg, uint8_t value)
{
  if (reg != BMP180_REG_CTRL_MEAS)
  {
    return;
  }
  if (value == BMP180_CMD_TEMP)
  {
    r[0xF6] = (uint8_t)(bmp_ut >> 8);
    r[0xF7] = (uint8_t)bmp_ut;
  }
  else
  {
    uint8_t  oss = (uint8_t)(value >> 6);
    uint32_t raw = (uint32_t)bmp_up << (8U - oss);
    r[0xF6] = (uint8_t)(raw >> 16);
    r[0xF7] = (uint8_t)(raw >> 8);
    r[0xF8] = (uint8_t)raw;
  }
}

dev_status_t fake_sensors_mpu_attach(void)
{
  uint8_t *r = fake_i2c_add_device(MPU6050_I2C_ADDR);

  r[MPU6050_REG_WHO_AM_I] = MPU6050_WHO_AM_I_VALUE;
  fake_sensors_mpu_set_accel(0, 0, MPU6050_ACCEL_LSB_PER_G);
  app_devices.mpu_status = mpu6050_init(&app_devices.mpu, Platform_SensorI2c(), MPU6050_I2C_ADDR);
  return app_devices.mpu_status;
}

void fake_sensors_mpu_set_accel(int16_t ax, int16_t ay, int16_t az)
{
  uint8_t *r = fake_i2c_device_regs(MPU6050_I2C_ADDR);
  const int16_t v[3] = { ax, ay, az };

  if (r == NULL)
  {
    return;
  }
  for (unsigned i = 0U; i < 3U; i++)
  {
    r[MPU6050_REG_ACCEL_XOUT_H + (2U * i)]      = (uint8_t)((uint16_t)v[i] >> 8);
    r[MPU6050_REG_ACCEL_XOUT_H + (2U * i) + 1U] = (uint8_t)v[i];
  }
}

dev_status_t fake_sensors_bmp_attach(void)
{
  uint8_t *r = fake_i2c_add_device(BMP180_I2C_ADDR);

  r[BMP180_REG_ID] = BMP180_CHIP_ID;
  memcpy(&r[BMP180_REG_CALIB], bmp_ds_calib_raw, sizeof(bmp_ds_calib_raw));
  fake_i2c_set_write_hook(BMP180_I2C_ADDR, bmp180_chip_hook);
  fake_sensors_bmp_set_raw(FAKE_BMP180_DS_UT, FAKE_BMP180_DS_UP);
  app_devices.baro_status = bmp180_init(&app_devices.baro, Platform_SensorI2c(), Platform_DelayMs, 0U);
  return app_devices.baro_status;
}

void fake_sensors_bmp_set_raw(int32_t ut, int32_t up)
{
  bmp_ut = ut;
  bmp_up = up;
}
