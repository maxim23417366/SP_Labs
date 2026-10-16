/**
  ******************************************************************************
  * @file    bmp180.c
  * @brief   Драйвер BMP180: ідентифікація, калібрування, вимірювання, компенсація.
  ******************************************************************************
  */
#include "bmp180.h"

/* Максимальний час перетворення для oss 0..3 (4.5 / 7.5 / 13.5 / 25.5 мс з datasheet), округлено вгору */
static const uint8_t pressure_wait_ms[BMP180_OSS_MAX + 1U] = { 5U, 8U, 14U, 26U };
#define TEMP_WAIT_MS  5U

static dev_status_t write_reg(bmp180_t *dev, uint8_t reg, uint8_t value)
{
  const uint8_t buf[2] = { reg, value };

  return dev->bus->write(dev->bus->ctx, BMP180_I2C_ADDR, buf, sizeof(buf));
}

static dev_status_t read_regs(bmp180_t *dev, uint8_t reg, uint8_t *data, size_t len)
{
  return dev->bus->read_reg(dev->bus->ctx, BMP180_I2C_ADDR, reg, data, len);
}

static uint16_t be16(const uint8_t *p)
{
  return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

int bmp180_parse_calib(const uint8_t raw[BMP180_CALIB_LEN], bmp180_calib_t *calib)
{
  uint16_t w[BMP180_CALIB_LEN / 2U];

  for (size_t i = 0U; i < BMP180_CALIB_LEN / 2U; i++)
  {
    w[i] = be16(&raw[2U * i]);
    if ((w[i] == 0x0000U) || (w[i] == 0xFFFFU))
    {
      return 0;
    }
  }

  calib->ac1 = (int16_t)w[0];
  calib->ac2 = (int16_t)w[1];
  calib->ac3 = (int16_t)w[2];
  calib->ac4 = w[3];
  calib->ac5 = w[4];
  calib->ac6 = w[5];
  calib->b1  = (int16_t)w[6];
  calib->b2  = (int16_t)w[7];
  calib->mb  = (int16_t)w[8];
  calib->mc  = (int16_t)w[9];
  calib->md  = (int16_t)w[10];
  return 1;
}

/* Datasheet записує алгоритм через "/ 2^n". Для від'ємних чисел це саме
 * арифметичний зсув (округлення до -inf), а не ділення C (до нуля): з діленням
 * приклад із datasheet дає 69965 Па замість 69964. GCC для ARM і x86
 * гарантує арифметичний зсув знакових чисел. */
int32_t bmp180_compensate_temp(const bmp180_calib_t *c, int32_t ut, int32_t *b5)
{
  int32_t x1 = (int32_t)(((int64_t)(ut - (int32_t)c->ac6) * c->ac5) >> 15);
  int32_t x2 = ((int32_t)c->mc * 2048) / (x1 + c->md);

  *b5 = x1 + x2;
  return (*b5 + 8) >> 4;
}

int32_t bmp180_compensate_pressure(const bmp180_calib_t *c, int32_t up, int32_t b5, uint8_t oss)
{
  int32_t  b6 = b5 - 4000;
  int32_t  x1 = (c->b2 * ((b6 * b6) >> 12)) >> 11;
  int32_t  x2 = (c->ac2 * b6) >> 11;
  int32_t  x3 = x1 + x2;
  int32_t  b3 = ((((int32_t)c->ac1 * 4 + x3) << oss) + 2) >> 2;
  uint32_t b4;
  uint32_t b7;
  int32_t  p;

  x1 = (c->ac3 * b6) >> 13;
  x2 = (c->b1 * ((b6 * b6) >> 12)) >> 16;
  x3 = ((x1 + x2) + 2) >> 2;
  b4 = ((uint32_t)c->ac4 * (uint32_t)(x3 + 32768)) >> 15;
  b7 = ((uint32_t)up - (uint32_t)b3) * (uint32_t)(50000U >> oss);

  if (b7 < 0x80000000UL)
  {
    p = (int32_t)((b7 * 2U) / b4);
  }
  else
  {
    p = (int32_t)((b7 / b4) * 2U);
  }

  x1 = (p >> 8) * (p >> 8);
  x1 = (x1 * 3038) >> 16;
  x2 = (-7357 * p) >> 16;
  return p + ((x1 + x2 + 3791) >> 4);
}

dev_status_t bmp180_init(bmp180_t *dev, const dev_i2c_bus_t *bus,
                         dev_delay_ms_fn delay_ms, uint8_t oss)
{
  uint8_t id = 0U;
  uint8_t raw[BMP180_CALIB_LEN];
  dev_status_t st;

  if ((dev == NULL) || (bus == NULL) || (delay_ms == NULL) || (oss > BMP180_OSS_MAX))
  {
    return DEV_ERR_PARAM;
  }
  dev->bus      = bus;
  dev->delay_ms = delay_ms;
  dev->oss      = oss;

  st = read_regs(dev, BMP180_REG_ID, &id, 1U);
  if (st != DEV_OK)
  {
    return st;
  }
  if (id != BMP180_CHIP_ID)
  {
    return DEV_ERR_ID;
  }

  st = read_regs(dev, BMP180_REG_CALIB, raw, sizeof(raw));
  if (st != DEV_OK)
  {
    return st;
  }
  return bmp180_parse_calib(raw, &dev->calib) ? DEV_OK : DEV_ERR_STATE;
}

dev_status_t bmp180_read_raw(bmp180_t *dev, int32_t *ut, int32_t *up)
{
  uint8_t buf[3];
  dev_status_t st;

  st = write_reg(dev, BMP180_REG_CTRL_MEAS, BMP180_CMD_TEMP);
  if (st != DEV_OK)
  {
    return st;
  }
  dev->delay_ms(TEMP_WAIT_MS);
  st = read_regs(dev, BMP180_REG_OUT_MSB, buf, 2U);
  if (st != DEV_OK)
  {
    return st;
  }
  *ut = (int32_t)be16(buf);

  st = write_reg(dev, BMP180_REG_CTRL_MEAS, (uint8_t)(BMP180_CMD_PRESSURE | (dev->oss << 6)));
  if (st != DEV_OK)
  {
    return st;
  }
  dev->delay_ms(pressure_wait_ms[dev->oss]);
  st = read_regs(dev, BMP180_REG_OUT_MSB, buf, 3U);
  if (st != DEV_OK)
  {
    return st;
  }
  *up = (int32_t)((((uint32_t)buf[0] << 16) | ((uint32_t)buf[1] << 8) | buf[2]) >> (8U - dev->oss));
  return DEV_OK;
}

dev_status_t bmp180_read(bmp180_t *dev, int32_t *temp_dc, int32_t *pressure_pa)
{
  int32_t ut;
  int32_t up;
  int32_t b5;
  dev_status_t st = bmp180_read_raw(dev, &ut, &up);

  if (st != DEV_OK)
  {
    return st;
  }
  *temp_dc     = bmp180_compensate_temp(&dev->calib, ut, &b5);
  *pressure_pa = bmp180_compensate_pressure(&dev->calib, up, b5, dev->oss);
  return DEV_OK;
}
