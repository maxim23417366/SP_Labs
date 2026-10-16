/**
  ******************************************************************************
  * @file    platform_bus.c
  * @brief   Реалізація dev_i2c_bus_t / dev_spi_bus_t над STM32 HAL.
  *
  *          Це єдине місце, де драйвери з Devices/ «зустрічаються» з HAL:
  *          тут HAL_StatusTypeDef перетворюється на dev_status_t, а CS# флешки
  *          керується через GPIO. Тонкий шар без логіки — перевіряється на платі
  *          разом із записами логічного аналізатора, а не хостовими тестами.
  ******************************************************************************
  */
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

/* ---- I2C ------------------------------------------------------------------ */

static dev_status_t i2c_write(void *ctx, uint8_t addr7, const uint8_t *data, size_t len)
{
  I2C_HandleTypeDef *hi2c = ctx;

  if (len > UINT16_MAX)
  {
    return DEV_ERR_PARAM;
  }
  /* HAL приймає 8-бітну адресу (7-бітна, зсунута вліво) і не-const буфер */
  return from_hal_i2c(hi2c, HAL_I2C_Master_Transmit(hi2c, (uint16_t)(addr7 << 1), (uint8_t *)data,
                                                    (uint16_t)len, BOARD_I2C_TIMEOUT_MS));
}

static dev_status_t i2c_read_reg(void *ctx, uint8_t addr7, uint8_t reg, uint8_t *data, size_t len)
{
  I2C_HandleTypeDef *hi2c = ctx;

  if (len > UINT16_MAX)
  {
    return DEV_ERR_PARAM;
  }
  /* START, addr+W, reg, repeated START, addr+R, дані, STOP */
  return from_hal_i2c(hi2c, HAL_I2C_Mem_Read(hi2c, (uint16_t)(addr7 << 1), reg, I2C_MEMADD_SIZE_8BIT,
                                             data, (uint16_t)len, BOARD_I2C_TIMEOUT_MS));
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
