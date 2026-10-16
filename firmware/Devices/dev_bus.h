/**
  ******************************************************************************
  * @file    dev_bus.h
  * @brief   Інтерфейс шин для драйверів пристроїв (контракт напрямів 5 → 6).
  *
  *          Драйвери з Devices/ працюють із залізом лише через ці структури
  *          і не знають про HAL_I2C_Mem_Read чи хендли CubeMX. На платі
  *          структури заповнює Platform/ (обгортки над HAL), у тестах на ПК —
  *          test doubles з tests/doubles/. Цей файл не включає жодного
  *          заголовка HAL і не повинен.
  ******************************************************************************
  */
#ifndef DEV_BUS_H
#define DEV_BUS_H

#include <stddef.h>
#include <stdint.h>

/* Спільні коди результату шин і драйверів */
typedef enum
{
  DEV_OK = 0,
  DEV_ERR_NACK,     /* пристрій не підтвердив адресу або байт (немає ACK) */
  DEV_ERR_TIMEOUT,  /* шина або пристрій не відповіли вчасно */
  DEV_ERR_BUS,      /* інша помилка шини: arbitration lost, BUSY, збій SPI */
  DEV_ERR_ID,       /* регістр ідентифікації містить не те, що очікує драйвер */
  DEV_ERR_STATE,    /* пристрій у неочікуваному стані (наприклад, WEL не встановився) */
  DEV_ERR_PARAM     /* некоректні аргументи виклику */
} dev_status_t;

/* I2C, 7-бітна адреса. Реалізація сама формує START/STOP. */
typedef struct
{
  void *ctx;

  /* START, addr+W, data[0..len-1], STOP. Для запису регістру data[0] — номер регістру. */
  dev_status_t (*write)(void *ctx, uint8_t addr7, const uint8_t *data, size_t len);

  /* START, addr+W, reg, repeated START, addr+R, data[0..len-1], STOP */
  dev_status_t (*read_reg)(void *ctx, uint8_t addr7, uint8_t reg, uint8_t *data, size_t len);
} dev_i2c_bus_t;

/* SPI master з програмним CS# (NSS не використовується). Один кадр —
 * select(), один або кілька transfer(), deselect(). */
typedef struct
{
  void *ctx;

  void (*select)(void *ctx);     /* CS# -> 0 */
  void (*deselect)(void *ctx);   /* CS# -> 1 */

  /* Повнодуплексний обмін len байтами. tx == NULL — передаються 0xFF,
   * rx == NULL — прийняті байти відкидаються. */
  dev_status_t (*transfer)(void *ctx, const uint8_t *tx, uint8_t *rx, size_t len);
} dev_spi_bus_t;

/* Затримка в мілісекундах (HAL_Delay на платі, фіктивний час у тестах) */
typedef void (*dev_delay_ms_fn)(uint32_t ms);

#endif /* DEV_BUS_H */
