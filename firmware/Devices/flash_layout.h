/**
  ******************************************************************************
  * @file    flash_layout.h
  * @brief   Карта пам'яті W25Q64 для SmartLock: адреси областей журналу і кадрів.
  *
  *          Джерело — docs/lab_3/flash-layout.md, розділ 1. Будь-яка зміна тут
  *          змінює формат даних у Flash: тоді треба збільшити
  *          FLASH_LAYOUT_VERSION, і прошивка відформатує журнал при старті.
  *
  *            0x000000  службовий сектор (опис розмітки)
  *            0x001000  журнал подій: 15 секторів, кільце, записи по 32 Б
  *            0x010000  кадри: 126 слотів по 64 КБ
  *            0x7F0000  резерв
  *            0x7FF000  стендовий тестовий сектор (команди w, v, f)
  ******************************************************************************
  */
#ifndef FLASH_LAYOUT_H
#define FLASH_LAYOUT_H

#include "w25q64.h"

#define FLASH_LAYOUT_VERSION      1U

/* Службовий сектор */
#define FLASH_SB_ADDR             0x000000UL
#define FLASH_SB_MAGIC            0x504D4C53UL   /* "SLMP" */
#define FLASH_SB_SIZE             32U

/* Журнал подій */
#define FLASH_LOG_BASE            0x001000UL
#define FLASH_LOG_SECTORS         15U
#define FLASH_LOG_END             (FLASH_LOG_BASE + (FLASH_LOG_SECTORS * W25Q64_SECTOR_SIZE))
#define FLASH_LOG_RECORD_SIZE     32U
#define FLASH_LOG_PER_SECTOR      (W25Q64_SECTOR_SIZE / FLASH_LOG_RECORD_SIZE)    /* 128 */
#define FLASH_LOG_CAPACITY        (FLASH_LOG_SECTORS * FLASH_LOG_PER_SECTOR)       /* 1920 */

/* Кадри */
#define FLASH_FRAME_BASE          0x010000UL
#define FLASH_FRAME_SLOT_SIZE     0x10000UL
#define FLASH_FRAME_SLOTS         126U
#define FLASH_FRAME_END           (FLASH_FRAME_BASE + (FLASH_FRAME_SLOTS * FLASH_FRAME_SLOT_SIZE))
#define FLASH_FRAME_DATA_OFFSET   0x100UL                                          /* сторінка 1 слота */
#define FLASH_FRAME_MAX_DATA      (FLASH_FRAME_SLOT_SIZE - FLASH_FRAME_DATA_OFFSET) /* 65 280 Б */

/* Стендовий сектор: його робоча логіка не чіпає */
#define FLASH_BENCH_SECTOR        (W25Q64_SIZE_BYTES - W25Q64_SECTOR_SIZE)         /* 0x7FF000 */

/* Області не перетинаються і вирівняні на сектор */
_Static_assert(FLASH_LOG_BASE >= FLASH_SB_ADDR + W25Q64_SECTOR_SIZE, "журнал після службового сектора");
_Static_assert(FLASH_LOG_END <= FLASH_FRAME_BASE, "журнал перед кадрами");
_Static_assert(FLASH_FRAME_END <= FLASH_BENCH_SECTOR, "кадри перед стендовим сектором");
_Static_assert((FLASH_FRAME_BASE % FLASH_FRAME_SLOT_SIZE) == 0U, "слоти вирівняні на блок 64 КБ");
_Static_assert((W25Q64_PAGE_SIZE % FLASH_LOG_RECORD_SIZE) == 0U, "запис не перетинає межу сторінки");

#endif /* FLASH_LAYOUT_H */
