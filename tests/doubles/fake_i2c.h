/**
  * @file    fake_i2c.h
  * @brief   Fake I2C-шини: робоча модель регістрів пристроїв для тестів драйверів.
  *
  *          Це саме fake, а не mock: шина поводиться як справжня (регістри
  *          з автоінкрементом адреси, NACK від відсутньої адреси), і тест
  *          перевіряє переважно результат драйвера. Журнал транзакцій дає
  *          змогу додатково звірити байти «на дроті» з записом аналізатора.
  */
#ifndef FAKE_I2C_H
#define FAKE_I2C_H

#include "dev_bus.h"

#define FAKE_I2C_MAX_DEVICES  8U
#define FAKE_I2C_LOG_SIZE     64U
#define FAKE_I2C_WIRE_MAX     40U

/* Викликається на кожен записаний у регістр байт — так тест імітує реакцію
 * чипа (наприклад, BMP180 кладе результат вимірювання в 0xF6..0xF8). */
typedef void (*fake_i2c_write_hook_t)(uint8_t *regs, uint8_t reg, uint8_t value);

typedef struct
{
  uint8_t      addr7;
  uint8_t      is_read;
  dev_status_t result;
  /* Байти в тому порядку, як їх видно на шині (декодер PulseView/Logic 2):
   * адресний байт (addr << 1 | R/W), потім дані. Для read_reg — addr+W, reg,
   * addr+R, дані. При NACK адреси — лише адресний байт. */
  uint8_t      wire[FAKE_I2C_WIRE_MAX];
  size_t       wire_len;
} fake_i2c_xfer_t;

void fake_i2c_reset(void);
const dev_i2c_bus_t *fake_i2c_bus(void);

/* Додає пристрій і повертає його 256 регістрів для заповнення тестом. */
uint8_t *fake_i2c_add_device(uint8_t addr7);
void fake_i2c_set_write_hook(uint8_t addr7, fake_i2c_write_hook_t hook);

/* Транзакція з номером n (від 0) завершиться заданою помилкою. */
void fake_i2c_fail_at(size_t n, dev_status_t status);

size_t fake_i2c_log_count(void);
const fake_i2c_xfer_t *fake_i2c_log(size_t i);

#endif /* FAKE_I2C_H */
