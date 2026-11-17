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
/* ЛР3 (I04): такт першого фронту кожної події з EXTI (Platform_TakeEvent) */
static volatile uint32_t ev_t[PLATFORM_EV_COUNT];
static volatile uint8_t  ev_pending[PLATFORM_EV_COUNT];
static volatile uint32_t ev_overruns[PLATFORM_EV_COUNT];

static uint32_t uart_overruns;


/* ---- мікросекунди через DWT CYCCNT ------------------------------------------ */
static uint32_t cycles_per_us;

static inline uint32_t cyc(void)
{
  return DWT->CYCCNT;
}

uint32_t Platform_Cycles(void)
{
  return cyc();
}

uint32_t Platform_CyclesToUs(uint32_t cycles)
{
  /* до Platform_IoInit() дільник ще нульовий */
  return (cycles_per_us != 0U) ? (cycles / cycles_per_us) : 0U;
}

uint32_t Platform_CpuHz(void)
{
  return HAL_RCC_GetHCLKFreq();
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

  /* ЛР3 (I05): службові GPIO для логічного аналізатора (platform.h, PLATFORM_PROBE_*).
   * PB0 у firmware.ioc — SPARE_PB0 (вхід): тут він стає виходом разом з PB1/PB2/PB12. */
  HAL_GPIO_WritePin(GPIOB, GPIO_PIN_0 | GPIO_PIN_1 | GPIO_PIN_2 | GPIO_PIN_12, GPIO_PIN_RESET);
  g.Pin   = GPIO_PIN_0 | GPIO_PIN_1 | GPIO_PIN_2 | GPIO_PIN_12;
  g.Mode  = GPIO_MODE_OUTPUT_PP;
  g.Pull  = GPIO_NOPULL;
  g.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
  HAL_GPIO_Init(GPIOB, &g);

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
/* Усі звернення до I2C1 — через board_i2c_begin/end() (platform_bus.c):
 * при завислій шині відповідь DEV_ERR_BUS без 25-мс очікування BUSY у HAL,
 * а після помилки шини шина відновлюється для наступного звернення. */

/* HAL_ERROR тут — NACK або збій шини; розрізняємо за кодом помилки хендла */
static dev_status_t sccb_status(HAL_StatusTypeDef st)
{
  if (st == HAL_OK)
  {
    return DEV_OK;
  }
  if ((st == HAL_ERROR) && ((HAL_I2C_GetError(BOARD_SENSOR_I2C) & HAL_I2C_ERROR_AF) != 0U))
  {
    return DEV_ERR_NACK;
  }
  return (st == HAL_TIMEOUT) ? DEV_ERR_TIMEOUT : DEV_ERR_BUS;
}

dev_status_t Platform_I2cProbe(uint8_t addr7)
{
  HAL_StatusTypeDef st;
  dev_status_t r = board_i2c_begin();

  if (r != DEV_OK)
  {
    return r;
  }
  st = HAL_I2C_IsDeviceReady(BOARD_SENSOR_I2C, (uint16_t)(addr7 << 1), 1U, BOARD_I2C_TIMEOUT_MS);
  if (st == HAL_OK)
  {
    return DEV_OK;
  }
  /* IsDeviceReady на відсутній адресі повертає HAL_ERROR (NACK) — шину не чіпаємо */
  return board_i2c_end((st == HAL_BUSY) ? DEV_ERR_BUS :
                       (st == HAL_TIMEOUT) ? DEV_ERR_TIMEOUT : DEV_ERR_NACK);
}

/* Повертає DEV_ERR_NACK для будь-якої помилки — як і раніше (камера відповідає
 * не завжди), але після збою шини встигає її відновити */
dev_status_t Platform_SccbRead(uint8_t addr7, uint8_t reg, uint8_t *val)
{
  uint16_t a = (uint16_t)(addr7 << 1);
  dev_status_t r = board_i2c_begin();

  if (r != DEV_OK)
  {
    return r;
  }
  r = board_i2c_end(sccb_status(HAL_I2C_Master_Transmit(BOARD_SENSOR_I2C, a, &reg, 1U,
                                                        SCCB_TIMEOUT_MS)));
  if (r != DEV_OK)
  {
    return DEV_ERR_NACK;
  }
  /* SCCB: на 9-му біті читання ведений не зобов'язаний давати ACK — HAL це
   * переживає, бо для останнього байта сам видає NACK і STOP */
  r = board_i2c_begin();
  if (r != DEV_OK)
  {
    return r;
  }
  r = board_i2c_end(sccb_status(HAL_I2C_Master_Receive(BOARD_SENSOR_I2C, a, val, 1U,
                                                       SCCB_TIMEOUT_MS)));
  return (r == DEV_OK) ? DEV_OK : DEV_ERR_NACK;
}

dev_status_t Platform_SccbWrite(uint8_t addr7, uint8_t reg, uint8_t val)
{
  uint8_t buf[2] = { reg, val };
  dev_status_t r = board_i2c_begin();

  if (r != DEV_OK)
  {
    return r;
  }
  r = board_i2c_end(sccb_status(HAL_I2C_Master_Transmit(BOARD_SENSOR_I2C, (uint16_t)(addr7 << 1),
                                                        buf, 2U, SCCB_TIMEOUT_MS)));
  return (r == DEV_OK) ? DEV_OK : DEV_ERR_NACK;
}

uint32_t Platform_VsyncCount(void)
{
  return vsync_count;
}

/* Запам'ятати такт фронту. Викликається лише з ISR (одна пріоритетна група EXTI,
 * тож ISR не перериває одна одну). Порядок «спершу час, потім прапорець» разом із
 * бар'єром компілятора гарантує, що цикл, побачивши pending = 1, прочитає вже
 * записаний час: volatile сам по собі не забороняє переставити два записи. */
static void ev_mark(platform_event_t ev)
{
  if (ev_pending[ev] != 0U)
  {
    ev_overruns[ev]++;   /* попередню подію цикл ще не забрав — зберігаємо найстарішу */
    return;
  }
  ev_t[ev] = DWT->CYCCNT;
  __asm volatile ("" ::: "memory");
  ev_pending[ev] = 1U;
}

/* Усі EXTI з firmware.ioc приходять сюди (слабка функція HAL). Рахуємо кадри
 * камери; для B1, PIR і MPU_INT фіксуємо момент фронту (I04: затримка реакції). */
void HAL_GPIO_EXTI_Callback(uint16_t gpio_pin)
{
  if (gpio_pin == CAM_VSYNC_Pin)
  {
    vsync_count++;
  }
  else if (gpio_pin == B1_Pin)
  {
    ev_mark(PLATFORM_EV_LOCK_B1);
  }
  else if (gpio_pin == PIR_SENSOR_Pin)
  {
    ev_mark(PLATFORM_EV_PIR);
  }
  else if (gpio_pin == MPU_INT_Pin)
  {
    ev_mark(PLATFORM_EV_MPU_INT);
  }
}

int Platform_TakeEvent(platform_event_t ev, uint32_t *t_cycles)
{
  if ((ev >= PLATFORM_EV_COUNT) || (ev_pending[ev] == 0U))
  {
    return 0;
  }
  /* Якщо ISR спрацює між читанням часу і зняттям прапорця, вона лише збільшить
   * overrun і не перезапише ev_t — читання залишиться узгодженим. */
  if (t_cycles != NULL)
  {
    *t_cycles = ev_t[ev];
  }
  __asm volatile ("" ::: "memory");
  ev_pending[ev] = 0U;
  return 1;
}

uint32_t Platform_EventOverruns(platform_event_t ev)
{
  return (ev < PLATFORM_EV_COUNT) ? ev_overruns[ev] : 0U;
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
    uart_overruns++;   /* ЛР3 (I04): щонайменше один байт загублено (§6.2, потік команд) */
  }
  if (HAL_UART_Receive(BOARD_LOG_UART, &c, 1U, 0U) == HAL_OK)
  {
    return (int)c;
  }
  return -1;
}

uint32_t Platform_UartOverruns(void)
{
  return uart_overruns;
}
