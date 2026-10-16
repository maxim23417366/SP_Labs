/**
  * @file    fake_i2c.c
  * @brief   Реалізація fake I2C-шини.
  */
#include "fake_i2c.h"

#include <string.h>

typedef struct
{
  uint8_t              present;
  uint8_t              addr7;
  uint8_t              regs[256];
  fake_i2c_write_hook_t hook;
} fake_dev_t;

static fake_dev_t      devices[FAKE_I2C_MAX_DEVICES];
static fake_i2c_xfer_t xfer_log[FAKE_I2C_LOG_SIZE];
static size_t          xfer_count;
static size_t          fail_index;
static dev_status_t    fail_status;

static fake_dev_t *find(uint8_t addr7)
{
  for (size_t i = 0U; i < FAKE_I2C_MAX_DEVICES; i++)
  {
    if (devices[i].present && (devices[i].addr7 == addr7))
    {
      return &devices[i];
    }
  }
  return NULL;
}

static fake_i2c_xfer_t *log_begin(uint8_t addr7, uint8_t is_read)
{
  static fake_i2c_xfer_t overflow;
  fake_i2c_xfer_t *x = (xfer_count < FAKE_I2C_LOG_SIZE) ? &xfer_log[xfer_count] : &overflow;

  memset(x, 0, sizeof(*x));
  x->addr7   = addr7;
  x->is_read = is_read;
  x->wire[x->wire_len++] = (uint8_t)(addr7 << 1);   /* завжди починаємо з addr+W */
  return x;
}

static void wire_put(fake_i2c_xfer_t *x, uint8_t b)
{
  if (x->wire_len < FAKE_I2C_WIRE_MAX)
  {
    x->wire[x->wire_len++] = b;
  }
}

/* Загальна частина: помилка, заплановані тестом, і NACK відсутньої адреси */
static dev_status_t start(fake_i2c_xfer_t *x, fake_dev_t **dev)
{
  size_t n = xfer_count++;

  if (n == fail_index)
  {
    x->result = fail_status;
    return fail_status;
  }
  *dev = find(x->addr7);
  if (*dev == NULL)
  {
    x->result = DEV_ERR_NACK;
    return DEV_ERR_NACK;
  }
  return DEV_OK;
}

static dev_status_t fake_write(void *ctx, uint8_t addr7, const uint8_t *data, size_t len)
{
  fake_i2c_xfer_t *x = log_begin(addr7, 0U);
  fake_dev_t *dev = NULL;
  dev_status_t st = start(x, &dev);

  (void)ctx;
  if (st != DEV_OK)
  {
    return st;
  }
  for (size_t i = 0U; i < len; i++)
  {
    wire_put(x, data[i]);
  }
  if (len > 0U)
  {
    uint8_t reg = data[0];
    for (size_t i = 1U; i < len; i++, reg++)
    {
      dev->regs[reg] = data[i];
      if (dev->hook != NULL)
      {
        dev->hook(dev->regs, reg, data[i]);
      }
    }
  }
  x->result = DEV_OK;
  return DEV_OK;
}

static dev_status_t fake_read_reg(void *ctx, uint8_t addr7, uint8_t reg, uint8_t *data, size_t len)
{
  fake_i2c_xfer_t *x = log_begin(addr7, 1U);
  fake_dev_t *dev = NULL;
  dev_status_t st = start(x, &dev);

  (void)ctx;
  if (st != DEV_OK)
  {
    return st;
  }
  wire_put(x, reg);
  wire_put(x, (uint8_t)((addr7 << 1) | 1U));
  for (size_t i = 0U; i < len; i++)
  {
    data[i] = dev->regs[(uint8_t)(reg + i)];
    wire_put(x, data[i]);
  }
  x->result = DEV_OK;
  return DEV_OK;
}

static const dev_i2c_bus_t bus = { NULL, fake_write, fake_read_reg };

void fake_i2c_reset(void)
{
  memset(devices, 0, sizeof(devices));
  memset(xfer_log, 0, sizeof(xfer_log));
  xfer_count  = 0U;
  fail_index  = (size_t)-1;
  fail_status = DEV_OK;
}

const dev_i2c_bus_t *fake_i2c_bus(void)
{
  return &bus;
}

uint8_t *fake_i2c_add_device(uint8_t addr7)
{
  for (size_t i = 0U; i < FAKE_I2C_MAX_DEVICES; i++)
  {
    if (!devices[i].present)
    {
      devices[i].present = 1U;
      devices[i].addr7   = addr7;
      return devices[i].regs;
    }
  }
  return NULL;
}

void fake_i2c_set_write_hook(uint8_t addr7, fake_i2c_write_hook_t hook)
{
  fake_dev_t *dev = find(addr7);

  if (dev != NULL)
  {
    dev->hook = hook;
  }
}

void fake_i2c_fail_at(size_t n, dev_status_t status)
{
  fail_index  = n;
  fail_status = status;
}

size_t fake_i2c_log_count(void)
{
  return xfer_count;
}

const fake_i2c_xfer_t *fake_i2c_log(size_t i)
{
  return (i < FAKE_I2C_LOG_SIZE) ? &xfer_log[i] : NULL;
}
