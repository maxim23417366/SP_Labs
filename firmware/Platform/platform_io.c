/**
  ******************************************************************************
  * @file    platform_io.c
  * @brief   Стенд ЛР2: дискретні входи, DHT11, серво, SCCB камери, UART-команди.
  *
  *          Тут живе все, що потребує HAL або регістрів МК. App/ бачить лише
  *          функції з platform.h.
  ******************************************************************************
  */
#include "board.h"
#include "platform.h"

/* ---- піни, яких немає в firmware.ioc ЛР1 ------------------------------------ */
#define DOOR_PORT   GPIOB
#define DOOR_PIN    GPIO_PIN_4     /* D5, геркон ORD324 */
#define DHT_PORT    GPIOB
#define DHT_PIN     GPIO_PIN_6     /* D10, DHT11 DATA */

#define SCCB_TIMEOUT_MS  10U

static volatile uint32_t vsync_count;

/* ---- мікросекунди через DWT CYCCNT ------------------------------------------ */
static uint32_t cycles_per_us;

static inline uint32_t cyc(void)
{
  return DWT->CYCCNT;
}

static void delay_us(uint32_t us)
{
  uint32_t start = cyc();
  uint32_t ticks = us * cycles_per_us;
  while ((cyc() - start) < ticks)
  {
  }
}

void Platform_IoInit(void)
{
  GPIO_InitTypeDef g = { 0 };

  __HAL_RCC_GPIOB_CLK_ENABLE();

  /* PB4 після скидання — NJTRST з підтяжкою. Ми працюємо по SWD, тому пін вільний. */
  g.Pin   = DOOR_PIN;
  g.Mode  = GPIO_MODE_INPUT;
  g.Pull  = GPIO_PULLUP;
  g.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(DOOR_PORT, &g);

  /* DHT11: open-drain, 1 = відпустити лінію (pull-up 1 кОм на модулі) */
  HAL_GPIO_WritePin(DHT_PORT, DHT_PIN, GPIO_PIN_SET);
  g.Pin   = DHT_PIN;
  g.Mode  = GPIO_MODE_OUTPUT_OD;
  g.Pull  = GPIO_NOPULL;
  g.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(DHT_PORT, &g);

  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
  DWT->CYCCNT = 0U;
  DWT->CTRL  |= DWT_CTRL_CYCCNTENA_Msk;
  cycles_per_us = HAL_RCC_GetHCLKFreq() / 1000000U;
}

static uint32_t pin(GPIO_TypeDef *port, uint16_t p)
{
  return (HAL_GPIO_ReadPin(port, p) == GPIO_PIN_SET) ? 1U : 0U;
}

uint32_t Platform_ReadInputs(void)
{
  uint32_t v = 0U;

  v |= pin(BTN_1_GPIO_Port, BTN_1_Pin)        ? PLATFORM_IN_BTN1   : 0U;
  v |= pin(BTN_2_GPIO_Port, BTN_2_Pin)        ? PLATFORM_IN_BTN2   : 0U;
  v |= pin(BTN_3_GPIO_Port, BTN_3_Pin)        ? PLATFORM_IN_BTN3   : 0U;
  v |= pin(BTN_4_GPIO_Port, BTN_4_Pin)        ? PLATFORM_IN_BTN4   : 0U;
  v |= pin(B1_GPIO_Port, B1_Pin)              ? PLATFORM_IN_LOCK   : 0U;   /* FR-02 */
  v |= pin(DOOR_PORT, DOOR_PIN)               ? PLATFORM_IN_DOOR   : 0U;
  v |= pin(PIR_SENSOR_GPIO_Port, PIR_SENSOR_Pin) ? PLATFORM_IN_PIR : 0U;
  v |= pin(MPU_INT_GPIO_Port, MPU_INT_Pin)    ? PLATFORM_IN_MPUINT : 0U;
  return v;
}

/* ---- DHT11 -------------------------------------------------------------------- */
/* Чекає, поки лінія стане level; повертає витрачені мкс або 0 при тайм-ауті. */
static uint32_t wait_level(uint32_t level, uint32_t timeout_us)
{
  uint32_t start = cyc();
  uint32_t limit = timeout_us * cycles_per_us;
  while (pin(DHT_PORT, DHT_PIN) != level)
  {
    if ((cyc() - start) > limit)
    {
      return 0U;
    }
  }
  return ((cyc() - start) / cycles_per_us) + 1U;
}

dev_status_t Platform_Dht11Capture(uint16_t high_us[40])
{
  dev_status_t st = DEV_OK;

  /* Старт: МК тримає LOW >= 18 мс, потім відпускає лінію */
  HAL_GPIO_WritePin(DHT_PORT, DHT_PIN, GPIO_PIN_RESET);
  HAL_Delay(20U);

  __disable_irq();   /* ~5 мс: переривання зіпсували б мікросекундні таймінги */
  HAL_GPIO_WritePin(DHT_PORT, DHT_PIN, GPIO_PIN_SET);
  delay_us(30U);

  /* Відповідь датчика: ~80 мкс LOW, ~80 мкс HIGH */
  if ((wait_level(0U, 100U) == 0U) || (wait_level(1U, 120U) == 0U) || (wait_level(0U, 120U) == 0U))
  {
    st = DEV_ERR_TIMEOUT;
  }

  for (uint32_t i = 0U; (i < 40U) && (st == DEV_OK); i++)
  {
    /* кожен біт: ~50 мкс LOW, потім HIGH 26–28 мкс (0) або ~70 мкс (1) */
    if (wait_level(1U, 80U) == 0U)
    {
      st = DEV_ERR_TIMEOUT;
      break;
    }
    uint32_t start = cyc();
    if (wait_level(0U, 120U) == 0U)
    {
      st = DEV_ERR_TIMEOUT;
      break;
    }
    high_us[i] = (uint16_t)((cyc() - start) / cycles_per_us);
  }
  __enable_irq();

  return st;
}

/* ---- серво --------------------------------------------------------------------- */
static uint32_t tim3_tick_hz(void)
{
  uint32_t clk = HAL_RCC_GetPCLK1Freq();
  if ((RCC->CFGR & RCC_CFGR_PPRE1) != RCC_CFGR_PPRE1_DIV1)
  {
    clk *= 2U;
  }
  return clk / (BOARD_SERVO_TIM->Instance->PSC + 1U);
}

void Platform_ServoSetPulseUs(uint32_t us)
{
  uint32_t ticks = (uint32_t)(((uint64_t)us * tim3_tick_hz()) / 1000000U);
  __HAL_TIM_SET_COMPARE(BOARD_SERVO_TIM, BOARD_SERVO_CHANNEL, ticks);
}

uint32_t Platform_ServoPulseUs(void)
{
  uint32_t ccr = __HAL_TIM_GET_COMPARE(BOARD_SERVO_TIM, BOARD_SERVO_CHANNEL);
  return (uint32_t)(((uint64_t)ccr * 1000000U) / tim3_tick_hz());
}

uint32_t Platform_ServoFreqHz(void)
{
  return tim3_tick_hz() / (BOARD_SERVO_TIM->Instance->ARR + 1U);
}

/* ---- I2C / SCCB ---------------------------------------------------------------- */
dev_status_t Platform_I2cProbe(uint8_t addr7)
{
  HAL_StatusTypeDef st = HAL_I2C_IsDeviceReady(BOARD_SENSOR_I2C, (uint16_t)(addr7 << 1), 1U,
                                               BOARD_I2C_TIMEOUT_MS);
  if (st == HAL_OK)
  {
    return DEV_OK;
  }
  return (st == HAL_BUSY) ? DEV_ERR_BUS : DEV_ERR_NACK;
}

dev_status_t Platform_SccbRead(uint8_t addr7, uint8_t reg, uint8_t *val)
{
  uint16_t a = (uint16_t)(addr7 << 1);

  if (HAL_I2C_Master_Transmit(BOARD_SENSOR_I2C, a, &reg, 1U, SCCB_TIMEOUT_MS) != HAL_OK)
  {
    return DEV_ERR_NACK;
  }
  /* SCCB: на 9-му біті читання ведений не зобов'язаний давати ACK — HAL це
   * переживає, бо для останнього байта сам видає NACK і STOP */
  if (HAL_I2C_Master_Receive(BOARD_SENSOR_I2C, a, val, 1U, SCCB_TIMEOUT_MS) != HAL_OK)
  {
    return DEV_ERR_NACK;
  }
  return DEV_OK;
}

dev_status_t Platform_SccbWrite(uint8_t addr7, uint8_t reg, uint8_t val)
{
  uint8_t buf[2] = { reg, val };

  return (HAL_I2C_Master_Transmit(BOARD_SENSOR_I2C, (uint16_t)(addr7 << 1), buf, 2U,
                                  SCCB_TIMEOUT_MS) == HAL_OK) ? DEV_OK : DEV_ERR_NACK;
}

uint32_t Platform_VsyncCount(void)
{
  return vsync_count;
}

/* Усі EXTI з firmware.ioc приходять сюди (слабка функція HAL). Рахуємо кадри
 * камери; PIR і MPU_INT App/ опитує через Platform_ReadInputs(). */
void HAL_GPIO_EXTI_Callback(uint16_t gpio_pin)
{
  if (gpio_pin == CAM_VSYNC_Pin)
  {
    vsync_count++;
  }
}

/* ---- UART ---------------------------------------------------------------------- */
int Platform_UartGetChar(void)
{
  uint8_t c;

  /* Скидання ORE на F4 — це читання SR і DR, тому робимо його лише тоді, коли
   * переповнення справді сталося, інакше з'їли б прийнятий символ */
  if (__HAL_UART_GET_FLAG(BOARD_LOG_UART, UART_FLAG_ORE))
  {
    __HAL_UART_CLEAR_OREFLAG(BOARD_LOG_UART);
  }
  if (HAL_UART_Receive(BOARD_LOG_UART, &c, 1U, 0U) == HAL_OK)
  {
    return (int)c;
  }
  return -1;
}
