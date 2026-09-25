/**
  ******************************************************************************
  * @file    periph_check.h
  * @brief   Перевірка ініціалізації периферії (ЛР1, крок 6).
  *
  *          Після MX_*_Init() підтверджує, що всі модулі в стані READY,
  *          тактування відповідає конфігурації CubeMX, а шини I2C1 / SPI2
  *          і PWM TIM3 генерують реальні сигнали для логічного аналізатора.
  *          Драйверів датчиків тут немає — лише ініціалізація і присутність.
  ******************************************************************************
  */
#ifndef PERIPH_CHECK_H
#define PERIPH_CHECK_H

#include <stdint.h>

#define PERIPH_CHECK_I2C_MAX_FOUND  16U

/* Результати перевірки. Глобальна змінна periph_check доступна в дебагері
 * (CubeIDE → Live Expressions), тож результат видно навіть без UART. */
typedef struct
{
  /* Ініціалізація: усі MX_*_Init() повернули HAL_OK і модулі в стані READY */
  uint8_t  init_ok;

  /* Тактування, прочитане з регістрів RCC */
  uint32_t sysclk_hz;
  uint32_t hclk_hz;
  uint32_t pclk1_hz;
  uint32_t pclk2_hz;
  uint8_t  clocks_ok;

  /* I2C1 */
  uint8_t  i2c_bus_busy;     /* шина «висить»: немає підтяжок або SDA/SCL на землі */
  uint8_t  i2c_found[PERIPH_CHECK_I2C_MAX_FOUND];
  uint8_t  i2c_found_count;
  uint8_t  mpu6050_present;  /* 0x68 */
  uint8_t  ov7670_present;   /* 0x21, відповідає лише за наявності XCLK */
  uint8_t  i2c_ok;

  /* SPI2 — W25Q64 */
  uint8_t  jedec_id[3];      /* очікується EF 40 17 */
  uint8_t  flash_ok;

  /* TIM3 CH1 — PWM для SG92R, значення розраховані з регістрів таймера */
  uint32_t pwm_freq_hz;
  uint32_t pwm_pulse_us;
  uint8_t  pwm_ok;

  uint8_t  all_ok;
  uint32_t loop_count;
} PeriphCheck_Result;

extern PeriphCheck_Result periph_check;

/* Разова перевірка всієї периферії. Викликати після всіх MX_*_Init(). */
void PeriphCheck_RunAll(void);

/* Викликати в while(1): кожні 500 мс повторює запит до MPU-6050 і JEDEC ID,
 * щоб на шинах постійно були сигнали для аналізатора, і блимає LD2. */
void PeriphCheck_Loop(void);

/* Індикація фатальної помилки з Error_Handler(): LD2 світиться постійно. */
void PeriphCheck_OnError(void) __attribute__((noreturn));

#endif /* PERIPH_CHECK_H */
