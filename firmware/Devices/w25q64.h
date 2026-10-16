/**
  ******************************************************************************
  * @file    w25q64.h
  * @brief   Драйвер SPI NOR flash Winbond W25Q64JV (8 МБ).
  *
  *          Джерело: Winbond W25Q64JV datasheet — розділи Status Registers,
  *          Instruction Set (06h, 05h, 02h, 03h, 20h, 9Fh) і AC Electrical
  *          Characteristics (tPP, tSE). Номери сторінок — у docs/pinout.md.
  ******************************************************************************
  */
#ifndef W25Q64_H
#define W25Q64_H

#include "dev_bus.h"

#define W25Q64_CMD_WRITE_ENABLE   0x06U
#define W25Q64_CMD_READ_STATUS1   0x05U
#define W25Q64_CMD_PAGE_PROGRAM   0x02U
#define W25Q64_CMD_READ_DATA      0x03U
#define W25Q64_CMD_SECTOR_ERASE   0x20U
#define W25Q64_CMD_JEDEC_ID       0x9FU

#define W25Q64_SR1_BUSY           0x01U
#define W25Q64_SR1_WEL            0x02U

#define W25Q64_JEDEC_MANUFACTURER 0xEFU   /* Winbond */
#define W25Q64_JEDEC_MEMORY_TYPE  0x40U
#define W25Q64_JEDEC_CAPACITY     0x17U   /* 2^23 байт = 64 Мбіт */

#define W25Q64_PAGE_SIZE          256U
#define W25Q64_SECTOR_SIZE        4096U
#define W25Q64_SIZE_BYTES         (8UL * 1024UL * 1024UL)

/* Максимальні часи з datasheet (tPP = 3 мс, tSE = 400 мс) */
#define W25Q64_PROGRAM_TIMEOUT_MS 3U
#define W25Q64_ERASE_TIMEOUT_MS   400U

typedef struct
{
  const dev_spi_bus_t *bus;
  dev_delay_ms_fn      delay_ms;
  uint8_t              jedec_id[3];
} w25q64_t;

/* Читає JEDEC ID і перевіряє, що це саме W25Q64 (EF 40 17). */
dev_status_t w25q64_init(w25q64_t *dev, const dev_spi_bus_t *bus, dev_delay_ms_fn delay_ms);

dev_status_t w25q64_read_jedec_id(w25q64_t *dev, uint8_t id[3]);
dev_status_t w25q64_read_status(w25q64_t *dev, uint8_t *sr1);

dev_status_t w25q64_read(w25q64_t *dev, uint32_t addr, uint8_t *data, size_t len);

/* Запис у межах однієї сторінки 256 байт: Write Enable → Page Program → очікування BUSY = 0.
 * Комірки мають бути попередньо стерті (NOR flash записує лише 1 → 0). */
dev_status_t w25q64_write_page(w25q64_t *dev, uint32_t addr, const uint8_t *data, size_t len);

/* Стирання сектора 4 КБ; addr має бути вирівняний на 4096. */
dev_status_t w25q64_erase_sector(w25q64_t *dev, uint32_t addr);

#endif /* W25Q64_H */
