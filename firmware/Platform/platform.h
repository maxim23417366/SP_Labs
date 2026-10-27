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

/* ---- Стенд ЛР2: входи, DHT11, серво, камера, команди з UART ---------------- */

/* Біти Platform_ReadInputs(): сирі рівні пінів (1 = HIGH). Кнопки й геркон
 * мають pull-up (у МК, у B1 — на платі), тому «натиснуто / замкнено» = 0. PIR: 1 = рух. */
#define PLATFORM_IN_BTN1   (1UL << 0)   /* червона, PA9  (D8)  */
#define PLATFORM_IN_BTN2   (1UL << 1)   /* зелена,  PA1  (A1)  */
#define PLATFORM_IN_BTN3   (1UL << 2)   /* синя,    PA4  (A2)  */
#define PLATFORM_IN_BTN4   (1UL << 3)   /* жовта,   PA7  (D11) */
#define PLATFORM_IN_LOCK   (1UL << 4)   /* кнопка LOCK = USER B1 на Nucleo, PC13 (замість тумблера на PB0) */
#define PLATFORM_IN_DOOR   (1UL << 5)   /* геркон ORD324,   PB4 (D5) */
#define PLATFORM_IN_PIR    (1UL << 6)   /* HC-SR501 OUT,    PA0 (A0) */
#define PLATFORM_IN_MPUINT (1UL << 7)   /* MPU-6050 INT,    PB5 (D4) */

/* Налаштовує те, чого немає в CubeMX-конфігурації ЛР1: PB4 (геркон) і PB6
 * (DHT11, open-drain), лічильник тактів DWT для мікросекундних таймінгів. */
void     Platform_IoInit(void);
uint32_t Platform_ReadInputs(void);

/* DHT11 на PB6 (D10): старт-імпульс, відповідь датчика, 40 тривалостей HIGH у мкс.
 * DEV_ERR_TIMEOUT — датчик не відповів або обірвав передачу. */
dev_status_t Platform_Dht11Capture(uint16_t high_us[40]);

/* Ширина імпульсу ШІМ серво (TIM3 CH1, PA6), мкс. Період 20 мс задає CubeMX. */
void     Platform_ServoSetPulseUs(uint32_t us);
uint32_t Platform_ServoPulseUs(void);
uint32_t Platform_ServoFreqHz(void);

/* SCCB (OV7670): запис регістру, потім окремою транзакцією читання — без
 * repeated START, як вимагає специфікація SCCB. */
dev_status_t Platform_SccbRead(uint8_t addr7, uint8_t reg, uint8_t *val);
dev_status_t Platform_SccbWrite(uint8_t addr7, uint8_t reg, uint8_t val);

/* Кількість фронтів VSYNC камери (PC8, EXTI) з моменту старту. */
uint32_t Platform_VsyncCount(void);

/* Скан однієї адреси I2C1 (START, addr+W, STOP). DEV_ERR_NACK — немає ACK. */
dev_status_t Platform_I2cProbe(uint8_t addr7);

/* Один прийнятий з USART2 символ або -1, якщо нічого не прийшло. */
int Platform_UartGetChar(void);

/* Вмикає/вимикає періодичний трафік ЛР1 (I2C + JEDEC кожні 500 мс). */
void Platform_SetLoopTraffic(int on);

#endif /* PLATFORM_H */
