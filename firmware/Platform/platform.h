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

/* Лічильник тактів ядра (DWT CYCCNT) для вимірювання тривалості операцій.
 * Різниця двох значень коректна і після одного переповнення (при 96 МГц —
 * інтервали до ~44 с). Platform_CyclesToUs() переводить різницю в мікросекунди. */
uint32_t Platform_Cycles(void);
uint32_t Platform_CyclesToUs(uint32_t cycles);

/* ЛР3 (I02): частота ядра HCLK, Гц (96 МГц) — для перерахунку періодів задач у такти */
uint32_t Platform_CpuHz(void);

/* ---- ЛР3 (I05): службові GPIO для логічного аналізатора (методичка §5.3) ---------
 * Усі чотири канали — на порту GPIOB, тому один запис у BSRR = один фронт.
 * Піни вільні за docs/pinout.md (PB0 звільнено в ЛР2, PB1/PB2/PB12 не
 * використовуються); налаштовує їх виходами Platform_IoInit(). */
#define PLATFORM_PROBE_LOOP     0U   /* PB0  (A3)      — прохід Super Loop        */
#define PLATFORM_PROBE_REACT    1U   /* PB1  (CN10-24) — FSM обробляє подію        */
#define PLATFORM_PROBE_CAMERA   2U   /* PB2  (CN10-22) — захоплення кадру (I09); до того задача security */
#define PLATFORM_PROBE_STORAGE  3U   /* PB12 (CN10-16) — запис кадру у Flash (I09); до того задача env  */
#define PLATFORM_PROBE_COUNT    4U

#if defined(STM32F411xE) && !defined(PLATFORM_PROBE_FUNCTIONS)
/* На платі — один запис у GPIOB->BSRR без HAL і без виклику функції
 * (RM0383: GPIOB = 0x40020400, BSRR = +0x18). Так platform.h лишається без
 * заголовків HAL, а вимір додає лише кілька тактів. */
#define PLATFORM_GPIOB_BSRR     (*(volatile uint32_t *)0x40020418UL)
#define PLATFORM_PROBE_PIN(ch)  ((ch) == 0U ? 0U : (ch) == 1U ? 1U : (ch) == 2U ? 2U : 12U)
#define PROBE_ON(ch)            (PLATFORM_GPIOB_BSRR = (1UL << PLATFORM_PROBE_PIN(ch)))
#define PROBE_OFF(ch)           (PLATFORM_GPIOB_BSRR = (1UL << (PLATFORM_PROBE_PIN(ch) + 16U)))
#else
/* На ПК (хостові тести) — функція, яку реалізує fake_platform. */
void Platform_ProbeWrite(unsigned ch, unsigned level);
#define PROBE_ON(ch)            Platform_ProbeWrite((ch), 1U)
#define PROBE_OFF(ch)           Platform_ProbeWrite((ch), 0U)
#endif

/* ---- ЛР3 (I06): пам'ять (методичка §5.4) -------------------------------------------- */

/* Заповнює зарезервований стек (_estack - _Min_Stack_Size .. SP) шаблоном
 * 0xDEADBEEF. Викликати першим рядком main(). */
void     Platform_StackPaint(void);
/* Найглибше використання стека з моменту фарбування, байт. Дорівнює
 * Platform_StackReservedBytes() — стек вийшов за резерв (див. docs/superloop.md §5). */
uint32_t Platform_StackUsedBytes(void);
uint32_t Platform_StackReservedBytes(void);

typedef struct
{
  uint32_t arena;   /* скільки heap забрано в системи (sbrk) — фактично пік */
  uint32_t used;    /* зайнято блоками malloc зараз */
  uint32_t free;    /* вільно всередині arena */
} platform_heap_t;

void Platform_HeapInfo(platform_heap_t *heap);

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
