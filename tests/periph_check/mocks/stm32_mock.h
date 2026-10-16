/**
  * @file    stm32_mock.h
  * @brief   Мінімальна імітація STM32 HAL для запуску periph_check.c на ПК.
  *
  *          Містить лише ті типи, макроси і функції, які використовує
  *          periph_check.c. Поведінка «заліза» задається структурою mock:
  *          які адреси відповідають на I2C, що повертає флешка, які частоти
  *          і значення регістрів таймера.
  */
#ifndef STM32_MOCK_H
#define STM32_MOCK_H

#include <stdint.h>

typedef enum
{
  HAL_OK      = 0x00U,
  HAL_ERROR   = 0x01U,
  HAL_BUSY    = 0x02U,
  HAL_TIMEOUT = 0x03U
} HAL_StatusTypeDef;

/* ---- GPIO ---------------------------------------------------------------- */
typedef struct
{
  uint32_t id;
} GPIO_TypeDef;

typedef enum
{
  GPIO_PIN_RESET = 0U,
  GPIO_PIN_SET
} GPIO_PinState;

typedef struct
{
  uint32_t Pin;
  uint32_t Mode;
  uint32_t Pull;
  uint32_t Speed;
  uint32_t Alternate;
} GPIO_InitTypeDef;

#define GPIO_PIN_2              ((uint16_t)0x0004)
#define GPIO_PIN_5              ((uint16_t)0x0020)
#define GPIO_PIN_10             ((uint16_t)0x0400)
#define GPIO_MODE_OUTPUT_PP     0x00000001U
#define GPIO_NOPULL             0x00000000U
#define GPIO_SPEED_FREQ_LOW     0x00000000U

extern GPIO_TypeDef mock_gpioa;
extern GPIO_TypeDef mock_gpiob;
#define GPIOA                   (&mock_gpioa)
#define GPIOB                   (&mock_gpiob)

#define __HAL_RCC_GPIOA_CLK_ENABLE()  do { } while (0)

void HAL_GPIO_Init(GPIO_TypeDef *port, GPIO_InitTypeDef *init);
void HAL_GPIO_WritePin(GPIO_TypeDef *port, uint16_t pin, GPIO_PinState state);
void HAL_GPIO_TogglePin(GPIO_TypeDef *port, uint16_t pin);

/* ---- RCC ----------------------------------------------------------------- */
typedef struct
{
  volatile uint32_t CFGR;
} RCC_TypeDef;

extern RCC_TypeDef mock_rcc;
#define RCC                     (&mock_rcc)

/* Ті самі значення, що й у stm32f411xe.h */
#define RCC_CFGR_PPRE1          (0x7UL << 10)
#define RCC_CFGR_PPRE1_DIV1     0x00000000U
#define RCC_CFGR_PPRE1_DIV2     0x00001000U

uint32_t HAL_RCC_GetSysClockFreq(void);
uint32_t HAL_RCC_GetHCLKFreq(void);
uint32_t HAL_RCC_GetPCLK1Freq(void);
uint32_t HAL_RCC_GetPCLK2Freq(void);

uint32_t HAL_GetTick(void);

/* ---- I2C ----------------------------------------------------------------- */
typedef enum
{
  HAL_I2C_STATE_RESET = 0x00U,
  HAL_I2C_STATE_READY = 0x20U
} HAL_I2C_StateTypeDef;

typedef struct
{
  HAL_I2C_StateTypeDef State;
} I2C_HandleTypeDef;

HAL_StatusTypeDef HAL_I2C_IsDeviceReady(I2C_HandleTypeDef *hi2c, uint16_t dev_address,
                                        uint32_t trials, uint32_t timeout);

/* ---- SPI ----------------------------------------------------------------- */
typedef enum
{
  HAL_SPI_STATE_RESET = 0x00U,
  HAL_SPI_STATE_READY = 0x01U
} HAL_SPI_StateTypeDef;

typedef struct
{
  HAL_SPI_StateTypeDef State;
} SPI_HandleTypeDef;

HAL_StatusTypeDef HAL_SPI_TransmitReceive(SPI_HandleTypeDef *hspi, uint8_t *tx, uint8_t *rx,
                                          uint16_t size, uint32_t timeout);

/* ---- TIM ----------------------------------------------------------------- */
typedef struct
{
  volatile uint32_t CR1;
  volatile uint32_t CCER;
  volatile uint32_t PSC;
  volatile uint32_t ARR;
  volatile uint32_t CCR1;
} TIM_TypeDef;

typedef enum
{
  HAL_TIM_STATE_RESET = 0x00U,
  HAL_TIM_STATE_READY = 0x01U
} HAL_TIM_StateTypeDef;

typedef struct
{
  TIM_TypeDef          *Instance;
  HAL_TIM_StateTypeDef  State;
} TIM_HandleTypeDef;

#define TIM_CHANNEL_1           0x00000000U
#define TIM_CR1_CEN             0x0001U
#define TIM_CCER_CC1E           0x0001U

HAL_StatusTypeDef HAL_TIM_PWM_Start(TIM_HandleTypeDef *htim, uint32_t channel);

/* ---- UART ---------------------------------------------------------------- */
typedef enum
{
  HAL_UART_STATE_RESET = 0x00U,
  HAL_UART_STATE_READY = 0x20U
} HAL_UART_StateTypeDef;

typedef struct
{
  HAL_UART_StateTypeDef gState;
} UART_HandleTypeDef;

#define HAL_MAX_DELAY           0xFFFFFFFFU

HAL_StatusTypeDef HAL_UART_Transmit(UART_HandleTypeDef *huart, const uint8_t *data,
                                    uint16_t size, uint32_t timeout);

/* ---- Хендли, які в прошивці створює CubeMX ------------------------------- */
extern I2C_HandleTypeDef  hi2c1;
extern SPI_HandleTypeDef  hspi2;
extern TIM_HandleTypeDef  htim3;
extern UART_HandleTypeDef huart2;

/* ---- Керування «залізом» з тестів ---------------------------------------- */
typedef struct
{
  /* I2C1 */
  uint8_t           i2c_ack[128];       /* 1 — пристрій на цій 7-бітній адресі відповідає */
  uint8_t           i2c_bus_busy;       /* шина «висить»: IsDeviceReady повертає HAL_BUSY */
  uint32_t          i2c_probe_count;
  uint8_t           i2c_min_addr;
  uint8_t           i2c_max_addr;

  /* SPI2 + CS флешки */
  uint8_t           jedec_id[3];
  HAL_StatusTypeDef spi_status;
  uint8_t           flash_cs_level;     /* поточний рівень FLASH_CS (PB10) */
  uint32_t          flash_transfers;
  uint32_t          flash_protocol_errors;  /* CS не опущений або команда не 0x9F */

  /* RCC */
  uint32_t          sysclk_hz;
  uint32_t          hclk_hz;
  uint32_t          pclk1_hz;
  uint32_t          pclk2_hz;

  /* TIM3 */
  HAL_StatusTypeDef pwm_start_status;

  /* USART2: що прошивка відправила через printf-хук __io_putchar() */
  uint8_t           uart_tx[64];
  uint32_t          uart_tx_count;
  const UART_HandleTypeDef *uart_last_handle;

  /* LD2 і час */
  uint32_t          tick_ms;
  uint32_t          led_toggles;
  uint8_t           led_level;
} MockHal;

extern MockHal     mock;
extern TIM_TypeDef mock_tim3;

/* Стан «усе підключено і налаштовано як у firmware.ioc». Час не скидається,
 * бо PeriphCheck_Loop() зберігає свої таймстемпи між викликами. */
void mock_reset(void);

#endif /* STM32_MOCK_H */
