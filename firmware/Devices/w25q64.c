/**
  ******************************************************************************
  * @file    w25q64.c
  * @brief   Драйвер W25Q64: JEDEC ID, читання, запис сторінки, стирання сектора.
  *
  *          Кожна інструкція — окремий кадр із утриманим CS#: CS# опускається
  *          перед байтом команди і піднімається після останнього байта.
  *          Запис і стирання виконуються лише після Write Enable (06h), а
  *          підняття CS# після 02h/20h запускає внутрішній цикл (BUSY = 1).
  ******************************************************************************
  */
#include "w25q64.h"

/* Один кадр: заголовок (команда + адреса) і необов'язкова фаза даних.
 * CS# піднімається завжди, навіть якщо обмін завершився помилкою. */
static dev_status_t frame(w25q64_t *dev, const uint8_t *hdr, size_t hdr_len,
                          const uint8_t *tx, uint8_t *rx, size_t len)
{
  const dev_spi_bus_t *bus = dev->bus;
  dev_status_t st;

  bus->select(bus->ctx);
  st = bus->transfer(bus->ctx, hdr, NULL, hdr_len);
  if ((st == DEV_OK) && (len > 0U))
  {
    st = bus->transfer(bus->ctx, tx, rx, len);
  }
  bus->deselect(bus->ctx);
  return st;
}

static void put_addr(uint8_t *hdr, uint8_t cmd, uint32_t addr)
{
  hdr[0] = cmd;
  hdr[1] = (uint8_t)(addr >> 16);
  hdr[2] = (uint8_t)(addr >> 8);
  hdr[3] = (uint8_t)addr;
}

dev_status_t w25q64_read_jedec_id(w25q64_t *dev, uint8_t id[3])
{
  const uint8_t cmd = W25Q64_CMD_JEDEC_ID;

  return frame(dev, &cmd, 1U, NULL, id, 3U);
}

dev_status_t w25q64_read_status(w25q64_t *dev, uint8_t *sr1)
{
  const uint8_t cmd = W25Q64_CMD_READ_STATUS1;

  return frame(dev, &cmd, 1U, NULL, sr1, 1U);
}

static dev_status_t wait_ready(w25q64_t *dev, uint32_t timeout_ms)
{
  uint8_t sr1 = 0U;

  for (uint32_t elapsed = 0U; ; elapsed++)
  {
    dev_status_t st = w25q64_read_status(dev, &sr1);
    if (st != DEV_OK)
    {
      return st;
    }
    if ((sr1 & W25Q64_SR1_BUSY) == 0U)
    {
      return DEV_OK;
    }
    if (elapsed >= timeout_ms)
    {
      return DEV_ERR_TIMEOUT;
    }
    dev->delay_ms(1U);
  }
}

/* Write Enable і перевірка, що флешка справді встановила WEL: інакше
 * наступна 02h/20h буде мовчки проігнорована (наприклад, при /WP = 0). */
static dev_status_t write_enable(w25q64_t *dev)
{
  const uint8_t cmd = W25Q64_CMD_WRITE_ENABLE;
  uint8_t sr1 = 0U;
  dev_status_t st = frame(dev, &cmd, 1U, NULL, NULL, 0U);

  if (st == DEV_OK)
  {
    st = w25q64_read_status(dev, &sr1);
  }
  if ((st == DEV_OK) && ((sr1 & W25Q64_SR1_WEL) == 0U))
  {
    st = DEV_ERR_STATE;
  }
  return st;
}

dev_status_t w25q64_init(w25q64_t *dev, const dev_spi_bus_t *bus, dev_delay_ms_fn delay_ms)
{
  dev_status_t st;

  if ((dev == NULL) || (bus == NULL) || (delay_ms == NULL))
  {
    return DEV_ERR_PARAM;
  }
  dev->bus      = bus;
  dev->delay_ms = delay_ms;

  st = w25q64_read_jedec_id(dev, dev->jedec_id);
  if (st != DEV_OK)
  {
    return st;
  }
  /* Без мікросхеми MISO «висить» і читається FF FF FF або 00 00 00 */
  if ((dev->jedec_id[0] != W25Q64_JEDEC_MANUFACTURER) ||
      (dev->jedec_id[1] != W25Q64_JEDEC_MEMORY_TYPE) ||
      (dev->jedec_id[2] != W25Q64_JEDEC_CAPACITY))
  {
    return DEV_ERR_ID;
  }
  return DEV_OK;
}

dev_status_t w25q64_read(w25q64_t *dev, uint32_t addr, uint8_t *data, size_t len)
{
  uint8_t hdr[4];

  if ((data == NULL) || (len == 0U) || (addr >= W25Q64_SIZE_BYTES) ||
      (len > W25Q64_SIZE_BYTES - addr))
  {
    return DEV_ERR_PARAM;
  }
  put_addr(hdr, W25Q64_CMD_READ_DATA, addr);
  return frame(dev, hdr, sizeof(hdr), NULL, data, len);
}

dev_status_t w25q64_write_page(w25q64_t *dev, uint32_t addr, const uint8_t *data, size_t len)
{
  uint8_t hdr[4];
  dev_status_t st;

  /* Перехід через межу сторінки флешка «загортає» на її початок і псує дані */
  if ((data == NULL) || (len == 0U) || (addr >= W25Q64_SIZE_BYTES) ||
      ((addr % W25Q64_PAGE_SIZE) + len > W25Q64_PAGE_SIZE))
  {
    return DEV_ERR_PARAM;
  }

  st = write_enable(dev);
  if (st != DEV_OK)
  {
    return st;
  }
  put_addr(hdr, W25Q64_CMD_PAGE_PROGRAM, addr);
  st = frame(dev, hdr, sizeof(hdr), data, NULL, len);
  if (st != DEV_OK)
  {
    return st;
  }
  return wait_ready(dev, W25Q64_PROGRAM_TIMEOUT_MS);
}

dev_status_t w25q64_erase_sector(w25q64_t *dev, uint32_t addr)
{
  uint8_t hdr[4];
  dev_status_t st;

  if ((addr >= W25Q64_SIZE_BYTES) || ((addr % W25Q64_SECTOR_SIZE) != 0U))
  {
    return DEV_ERR_PARAM;
  }

  st = write_enable(dev);
  if (st != DEV_OK)
  {
    return st;
  }
  put_addr(hdr, W25Q64_CMD_SECTOR_ERASE, addr);
  st = frame(dev, hdr, sizeof(hdr), NULL, NULL, 0U);
  if (st != DEV_OK)
  {
    return st;
  }
  return wait_ready(dev, W25Q64_ERASE_TIMEOUT_MS);
}
