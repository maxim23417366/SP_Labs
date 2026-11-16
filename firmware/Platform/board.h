/**
  ******************************************************************************
  * @file    board.h
  * @brief   Єдине місце знань про плату: яка периферія і які піни обслуговують
  *          кожен пристрій SmartLock.
  *
  *          Номери пінів тут не дублюються: вони приходять з main.h, де CubeMX
  *          генерує мітки GPIO з firmware.ioc (FLASH_CS_Pin тощо). Після
  *          переносу піна в CubeMX за docs/pinout.md цей файл міняти не треба.
  *          Включається лише з .c-файлів Platform/ — Devices/ і App/ його не бачать.
  ******************************************************************************
  */
#ifndef BOARD_H
#define BOARD_H

#include "main.h"
#include "i2c.h"
#include "spi.h"
#include "tim.h"
#include "usart.h"

/* I2C1: MPU-6050, GY-89 (L3GD20, LSM303D, BMP180), OV7670 (SCCB) */
#define BOARD_SENSOR_I2C        (&hi2c1)
#define BOARD_I2C_TIMEOUT_MS    10U
/* Піни I2C1 (Core/Src/i2c.c, HAL_I2C_MspInit): потрібні для відновлення шини,
 * коли лінії тимчасово переводяться в режим GPIO */
#define BOARD_I2C_SCL_PORT      GPIOB
#define BOARD_I2C_SCL_PIN       GPIO_PIN_8
#define BOARD_I2C_SDA_PORT      GPIOB
#define BOARD_I2C_SDA_PIN       GPIO_PIN_9

/* SPI2 + програмний CS#: W25Q64 */
#define BOARD_FLASH_SPI         (&hspi2)
#define BOARD_FLASH_CS_PORT     FLASH_CS_GPIO_Port
#define BOARD_FLASH_CS_PIN      FLASH_CS_Pin
#define BOARD_SPI_TIMEOUT_MS    10U

/* TIM3 CH1: ШІМ 50 Гц для SG92R */
#define BOARD_SERVO_TIM         (&htim3)
#define BOARD_SERVO_CHANNEL     TIM_CHANNEL_1

/* USART2 через ST-LINK VCP: лог і self-test, 115200 8N1 */
#define BOARD_LOG_UART          (&huart2)
#define BOARD_UART_TIMEOUT_MS   10U

/* ---- Внутрішнє для Platform/: охорона транзакцій I2C1 (platform_bus.c) ------------
 * Кожне звернення до hi2c1 з Platform/ обгортається так:
 *     st = board_i2c_begin();
 *     if (st == DEV_OK) { st = <виклик HAL>; st = board_i2c_end(st); }
 * begin() не пускає в HAL, якщо шина зависла (інакше HAL чекає BUSY 25 мс),
 * end() після помилки шини або тайм-ауту відновлює шину для наступного звернення. */
#include "dev_bus.h"

dev_status_t board_i2c_begin(void);
dev_status_t board_i2c_end(dev_status_t st);

#endif /* BOARD_H */
