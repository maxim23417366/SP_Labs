/**
  ******************************************************************************
  * @file    platform_bus.c
  * @brief   Реалізація dev_i2c_bus_t / dev_spi_bus_t над STM32 HAL.
  *
  *          Це єдине місце, де драйвери з Devices/ «зустрічаються» з HAL:
  *          тут HAL_StatusTypeDef перетворюється на dev_status_t, а CS# флешки
  *          керується через GPIO. Залежить від HAL, тому перевіряється на платі
  *          разом із записами логічного аналізатора, а не хостовими тестами.
  *
  *          ЛР3: кожне звернення до I2C1 охороняється board_i2c_begin/end() —
  *          автоматичне відновлення шини після збою (див. розділ нижче).
  ******************************************************************************
  */
#include <stdbool.h>
#include <string.h>

#include "board.h"
#include "platform.h"

static dev_status_t from_hal(HAL_StatusTypeDef st)
{
  switch (st)
  {
    case HAL_OK:      return DEV_OK;
    case HAL_TIMEOUT: return DEV_ERR_TIMEOUT;
    default:          return DEV_ERR_BUS;
  }
}

/* HAL повертає HAL_ERROR і для NACK, і для arbitration lost / bus error:
 * відрізняємо NACK за кодом помилки хендла (Acknowledge Failure). */
static dev_status_t from_hal_i2c(I2C_HandleTypeDef *hi2c, HAL_StatusTypeDef st)
{
  if ((st == HAL_ERROR) && ((HAL_I2C_GetError(hi2c) & HAL_I2C_ERROR_AF) != 0U))
  {
    return DEV_ERR_NACK;
  }
  return from_hal(st);
}

/* ---- I2C: відновлення шини -------------------------------------------------- */
/*
 * Що лікуємо (docs/lab_3/driver-audit.md, дефект 2; логи 10, 11, 15):
 *   - живлення модулів на мить зникло, разом із ним підтяжки: SCL і SDA падають
 *     у нуль, периферія I2C1 бачить «START» і виставляє BUSY. Після повернення
 *     живлення BUSY лишається, і кожен виклик HAL 25 мс чекає його зняття
 *     (I2C_TIMEOUT_BUSY_FLAG у stm32f4xx_hal_i2c.c) — звідси 26 мс на звернення
 *     і порожній скан до скидання МК;
 *   - ведений, у якого обірвалась транзакція, тримає SDA в нулі, чекаючи
 *     тактів, яких МК уже не дасть.
 * Як лікуємо: I2C1 вимикається, SCL і SDA стають GPIO open-drain, на SCL до
 * дев'яти тактів (ведений дочитує байт і відпускає SDA), потім умова STOP і
 * повторна ініціалізація HAL_I2C_Init(), у якій є програмне скидання SWRST —
 * воно знімає завислий BUSY. На 100 кГц це близько 0,1–0,2 мс.
 */

static platform_i2c_stats_t i2c_stats;

/* Активне очікування в мікросекундах на DWT CYCCNT (лічильник вмикає
 * Platform_IoInit(); якщо ще ні — вмикаємо тут, інакше цикл не завершився б) */
static void bus_delay_us(uint32_t us)
{
  uint32_t ticks = (HAL_RCC_GetHCLKFreq() / 1000000U) * us;
  uint32_t start;

  if ((DWT->CTRL & DWT_CTRL_CYCCNTENA_Msk) == 0U)
  {
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CTRL        |= DWT_CTRL_CYCCNTENA_Msk;
  }
  start = DWT->CYCCNT;
  while ((DWT->CYCCNT - start) < ticks)
  {
  }
}

/* Рівні ліній читаються з IDR і в режимі альтернативної функції */
static bool scl_high(void)
{
  return HAL_GPIO_ReadPin(BOARD_I2C_SCL_PORT, BOARD_I2C_SCL_PIN) == GPIO_PIN_SET;
}

static bool sda_high(void)
{
  return HAL_GPIO_ReadPin(BOARD_I2C_SDA_PORT, BOARD_I2C_SDA_PIN) == GPIO_PIN_SET;
}

static void scl_set(bool high)
{
  HAL_GPIO_WritePin(BOARD_I2C_SCL_PORT, BOARD_I2C_SCL_PIN, high ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

static void sda_set(bool high)
{
  HAL_GPIO_WritePin(BOARD_I2C_SDA_PORT, BOARD_I2C_SDA_PIN, high ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

/* Шину можна віддавати HAL: периферія готова, BUSY знято, обидві лінії в одиниці */
static bool i2c_usable(I2C_HandleTypeDef *hi2c)
{
  return (hi2c->State == HAL_I2C_STATE_READY) && !__HAL_I2C_GET_FLAG(hi2c, I2C_FLAG_BUSY) &&
         scl_high() && sda_high();
}

/* Bus clear (UM10204, розд. 3.1.16): до 9 тактів SCL, поки ведений не відпустить
 * SDA, потім STOP. Повертає true, якщо після цього обидві лінії в одиниці. */
static bool i2c_bus_clear(uint32_t half_us)
{
  GPIO_InitTypeDef g = { 0 };

  /* Спершу рівні «1», потім режим виходу — без зайвого фронту на лініях */
  scl_set(true);
  sda_set(true);
  g.Mode  = GPIO_MODE_OUTPUT_OD;
  g.Pull  = GPIO_NOPULL;
  g.Speed = GPIO_SPEED_FREQ_HIGH;
  g.Pin   = BOARD_I2C_SCL_PIN;
  HAL_GPIO_Init(BOARD_I2C_SCL_PORT, &g);
  g.Pin   = BOARD_I2C_SDA_PIN;
  HAL_GPIO_Init(BOARD_I2C_SDA_PORT, &g);
  bus_delay_us(half_us);

  for (uint32_t i = 0U; (i < 9U) && !sda_high(); i++)
  {
    scl_set(false);
    bus_delay_us(half_us);
    scl_set(true);
    bus_delay_us(half_us);
  }

  /* STOP: SDA 0 → 1 при SCL = 1 */
  scl_set(false);
  bus_delay_us(half_us);
  sda_set(false);
  bus_delay_us(half_us);
  scl_set(true);
  bus_delay_us(half_us);
  sda_set(true);
  bus_delay_us(half_us);

  return scl_high() && sda_high();
}

static dev_status_t i2c_recover(void)
{
  I2C_HandleTypeDef *hi2c = BOARD_SENSOR_I2C;
  /* Півперіод SCL як у робочому режимі: 100 кГц (CubeMX) → 5 мкс */
  uint32_t half_us = (hi2c->Init.ClockSpeed != 0U) ? (500000U / hi2c->Init.ClockSpeed) : 5U;
  bool ok;

  i2c_stats.attempts++;

  /* Вимикає I2C1 і повертає піни в стан скидання; State = RESET, тож
   * HAL_I2C_Init() нижче знову викличе HAL_I2C_MspInit() (піни AF4, тактування) */
  (void)HAL_I2C_DeInit(hi2c);
  ok = i2c_bus_clear((half_us != 0U) ? half_us : 1U);

  if (HAL_I2C_Init(hi2c) != HAL_OK)
  {
    ok = false;
  }
  if (ok && i2c_usable(hi2c))
  {
    i2c_stats.recovered++;
    return DEV_OK;
  }
  i2c_stats.failed++;
  return DEV_ERR_BUS;
}

/* Чекати, поки шина звільниться, не довше за передачу одного байта з ACK
 * (9 тактів SCL, ≈ 90 мкс на 100 кГц). Потрібно, бо після STOP попередньої
 * транзакції BUSY знімається не миттєво (стенд: без цього очікування
 * «відновлення» запускалось на справній шині щоразу, коли звернення йшли
 * одне за одним). HAL у тому самому випадку чекає до 25 мс. */
static bool i2c_settle(I2C_HandleTypeDef *hi2c)
{
  uint32_t byte_us = (hi2c->Init.ClockSpeed != 0U) ? (9000000U / hi2c->Init.ClockSpeed) : 90U;
  uint32_t ticks   = (HAL_RCC_GetHCLKFreq() / 1000000U) * ((byte_us != 0U) ? byte_us : 1U);
  uint32_t start;

  if (i2c_usable(hi2c))
  {
    return true;
  }
  bus_delay_us(0U);   /* вмикає DWT, якщо Platform_IoInit() ще не викликали */
  start = DWT->CYCCNT;
  while ((DWT->CYCCNT - start) < ticks)
  {
    if (i2c_usable(hi2c))
    {
      return true;
    }
  }
  return false;
}

dev_status_t board_i2c_begin(void)
{
  if (i2c_settle(BOARD_SENSOR_I2C) || (i2c_recover() == DEV_OK))
  {
    return DEV_OK;
  }
  i2c_stats.fast_fail++;   /* без живлення / підтяжок: відповідь за ~0,2 мс замість 26 мс */
  return DEV_ERR_BUS;
}

dev_status_t board_i2c_end(dev_status_t st)
{
  /* NACK — звичайна відповідь відсутнього пристрою, шина при цьому справна */
  if ((st == DEV_ERR_BUS) || (st == DEV_ERR_TIMEOUT))
  {
    (void)i2c_recover();
  }
  return st;
}

dev_status_t Platform_I2cRecover(void)
{
  return i2c_recover();
}

void Platform_I2cStats(platform_i2c_stats_t *out)
{
  if (out != NULL)
  {
    *out = i2c_stats;
  }
}

/* ---- I2C ------------------------------------------------------------------ */

static dev_status_t i2c_write(void *ctx, uint8_t addr7, const uint8_t *data, size_t len)
{
  I2C_HandleTypeDef *hi2c = ctx;
  dev_status_t st;

  if (len > UINT16_MAX)
  {
    return DEV_ERR_PARAM;
  }
  st = board_i2c_begin();
  if (st != DEV_OK)
  {
    return st;
  }
  /* HAL приймає 8-бітну адресу (7-бітна, зсунута вліво) і не-const буфер */
  st = from_hal_i2c(hi2c, HAL_I2C_Master_Transmit(hi2c, (uint16_t)(addr7 << 1), (uint8_t *)data,
                                                  (uint16_t)len, BOARD_I2C_TIMEOUT_MS));
  return board_i2c_end(st);
}

static dev_status_t i2c_read_reg(void *ctx, uint8_t addr7, uint8_t reg, uint8_t *data, size_t len)
{
  I2C_HandleTypeDef *hi2c = ctx;
  dev_status_t st;

  if (len > UINT16_MAX)
  {
    return DEV_ERR_PARAM;
  }
  st = board_i2c_begin();
  if (st != DEV_OK)
  {
    return st;
  }
  /* START, addr+W, reg, repeated START, addr+R, дані, STOP */
  st = from_hal_i2c(hi2c, HAL_I2C_Mem_Read(hi2c, (uint16_t)(addr7 << 1), reg, I2C_MEMADD_SIZE_8BIT,
                                           data, (uint16_t)len, BOARD_I2C_TIMEOUT_MS));
  return board_i2c_end(st);
}

static const dev_i2c_bus_t sensor_i2c = { BOARD_SENSOR_I2C, i2c_write, i2c_read_reg };

/* ---- SPI ------------------------------------------------------------------ */

static void spi_select(void *ctx)
{
  (void)ctx;
  HAL_GPIO_WritePin(BOARD_FLASH_CS_PORT, BOARD_FLASH_CS_PIN, GPIO_PIN_RESET);
}

static void spi_deselect(void *ctx)
{
  (void)ctx;
  HAL_GPIO_WritePin(BOARD_FLASH_CS_PORT, BOARD_FLASH_CS_PIN, GPIO_PIN_SET);
}

/* Таймаут HAL рахується на весь обмін, тож для довгих читань флешки
 * додаємо 1 мс на кожні 256 байт (із запасом навіть для 3 МГц SCK). */
static uint32_t spi_timeout_ms(uint16_t n)
{
  return BOARD_SPI_TIMEOUT_MS + n / 256U;
}

static dev_status_t spi_transfer(void *ctx, const uint8_t *tx, uint8_t *rx, size_t len)
{
  SPI_HandleTypeDef *hspi = ctx;
  static uint8_t ff[16];

  while (len > 0U)
  {
    uint16_t n = (len > UINT16_MAX) ? UINT16_MAX : (uint16_t)len;
    HAL_StatusTypeDef st;

    if ((tx != NULL) && (rx != NULL))
    {
      st = HAL_SPI_TransmitReceive(hspi, (uint8_t *)tx, rx, n, spi_timeout_ms(n));
    }
    else if (tx != NULL)
    {
      st = HAL_SPI_Transmit(hspi, (uint8_t *)tx, n, spi_timeout_ms(n));
    }
    else if (rx != NULL)
    {
      /* Контракт dev_bus.h: без tx по MOSI ідуть 0xFF. HAL_SPI_Receive у
       * повнодуплексному режимі відправив би вміст rx, тому заповнюємо його. */
      memset(rx, 0xFF, n);
      st = HAL_SPI_TransmitReceive(hspi, rx, rx, n, spi_timeout_ms(n));
    }
    else
    {
      n = (n > sizeof(ff)) ? (uint16_t)sizeof(ff) : n;
      memset(ff, 0xFF, sizeof(ff));
      st = HAL_SPI_Transmit(hspi, ff, n, spi_timeout_ms(n));
    }

    if (st != HAL_OK)
    {
      return from_hal(st);
    }
    len -= n;
    tx = (tx != NULL) ? tx + n : NULL;
    rx = (rx != NULL) ? rx + n : NULL;
  }
  return DEV_OK;
}

static const dev_spi_bus_t flash_spi = { BOARD_FLASH_SPI, spi_select, spi_deselect, spi_transfer };

/* ---- Публічний інтерфейс --------------------------------------------------- */

const dev_i2c_bus_t *Platform_SensorI2c(void)
{
  return &sensor_i2c;
}

const dev_spi_bus_t *Platform_FlashSpi(void)
{
  return &flash_spi;
}
