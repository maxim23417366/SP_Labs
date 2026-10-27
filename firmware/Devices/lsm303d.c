/**
  ******************************************************************************
  * @file    lsm303d.c
  * @brief   LSM303D: ідентифікація, ініціалізація акселерометра, читання, одиниці.
  ******************************************************************************
  */
#include "lsm303d.h"

#include <stddef.h>

static dev_status_t write_reg(lsm303d_t *dev, uint8_t reg, uint8_t value)
{
  const uint8_t buf[2] = { reg, value };

  return dev->bus->write(dev->bus->ctx, dev->addr7, buf, sizeof(buf));
}

dev_status_t lsm303d_init(lsm303d_t *dev, const dev_i2c_bus_t *bus, uint8_t addr7)
{
  uint8_t who = 0U;
  dev_status_t st;

  if ((dev == NULL) || (bus == NULL))
  {
    return DEV_ERR_PARAM;
  }
  dev->bus   = bus;
  dev->addr7 = addr7;

  st = bus->read_reg(bus->ctx, addr7, LSM303D_REG_WHO_AM_I, &who, 1U);
  if (st != DEV_OK)
  {
    return st;
  }
  if (who != LSM303D_WHO_AM_I_VALUE)
  {
    return DEV_ERR_ID;
  }

  /* Після скидання AODR = 0000 — акселерометр вимкнений */
  st = write_reg(dev, LSM303D_REG_CTRL1, LSM303D_CTRL1_50HZ_XYZ);
  if (st == DEV_OK)
  {
    st = write_reg(dev, LSM303D_REG_CTRL2, LSM303D_CTRL2_2G);
  }
  return st;
}

void lsm303d_parse(const uint8_t buf[LSM303D_SAMPLE_LEN], lsm303d_raw_t *raw)
{
  raw->ax = (int16_t)(((uint16_t)buf[1] << 8) | buf[0]);
  raw->ay = (int16_t)(((uint16_t)buf[3] << 8) | buf[2]);
  raw->az = (int16_t)(((uint16_t)buf[5] << 8) | buf[4]);
}

dev_status_t lsm303d_read_raw(lsm303d_t *dev, lsm303d_raw_t *raw)
{
  uint8_t buf[LSM303D_SAMPLE_LEN];
  dev_status_t st = dev->bus->read_reg(dev->bus->ctx, dev->addr7,
                                       (uint8_t)(LSM303D_REG_OUT_X_L_A | LSM303D_AUTO_INC),
                                       buf, sizeof(buf));
  if (st == DEV_OK)
  {
    lsm303d_parse(buf, raw);
  }
  return st;
}

void lsm303d_convert(const lsm303d_raw_t *raw, lsm303d_data_t *out)
{
  out->ax_mg = (int32_t)raw->ax * LSM303D_UG_PER_LSB_2G / 1000;
  out->ay_mg = (int32_t)raw->ay * LSM303D_UG_PER_LSB_2G / 1000;
  out->az_mg = (int32_t)raw->az * LSM303D_UG_PER_LSB_2G / 1000;
}
