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

#endif /* BOARD_H */
