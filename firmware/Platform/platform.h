/**
  ******************************************************************************
  * @file    platform.h
  * @brief   Публічний інтерфейс шару Platform для App/.
  *
  *          Не включає HAL і заголовки CubeMX: App/ отримує шини лише як
  *          dev_i2c_bus_t / dev_spi_bus_t і передає їх драйверам з Devices/.
  ******************************************************************************
  */
#ifndef PLATFORM_H
#define PLATFORM_H

#include <stdint.h>

#include "dev_bus.h"

/* I2C1: MPU-6050, GY-89, OV7670 */
const dev_i2c_bus_t *Platform_SensorI2c(void);

/* SPI2 з програмним CS#: W25Q64 */
const dev_spi_bus_t *Platform_FlashSpi(void);

void     Platform_DelayMs(uint32_t ms);
uint32_t Platform_Millis(void);

/* Перевірка периферії МК з ЛР1 (periph_check): ініціалізація, тактування,
 * скан I2C, JEDEC ID, ШІМ. Викликати один раз після MX_*_Init(). */
void Platform_BringUp(void);

/* Періодична частина periph_check: опитування шин і індикація на LD2. */
void Platform_Poll(void);

#endif /* PLATFORM_H */
