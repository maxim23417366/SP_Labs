/**
  * @file    mock_spi.h
  * @brief   Mock SPI-шини з програмним CS#: записує кадри і віддає заскриптовані відповіді.
  *
  *          Це mock, а не fake: тут немає моделі флешки. Тест сам задає, що
  *          прийде по MISO в кожному кадрі, а потім перевіряє, ЯКІ кадри і в
  *          ЯКОМУ порядку драйвер відправив (наприклад, 06h перед 02h). Кадр —
  *          усе, що передано між select() і deselect().
  */
#ifndef MOCK_SPI_H
#define MOCK_SPI_H

#include "dev_bus.h"

#define MOCK_SPI_MAX_FRAMES  32U
#define MOCK_SPI_FRAME_BYTES 300U

typedef struct
{
  uint8_t mosi[MOCK_SPI_FRAME_BYTES];
  size_t  len;
} mock_spi_frame_t;

void mock_spi_reset(void);
const dev_spi_bus_t *mock_spi_bus(void);

/* Що прийде по MISO в кадрі з номером frame (від 0); решта байтів — 0xFF.
 * Кадри з номером >= MOCK_SPI_MAX_FRAMES отримують сценарій останнього кадру. */
void mock_spi_set_miso(size_t frame, const uint8_t *bytes, size_t len);

/* transfer() у кадрі з номером frame поверне задану помилку. */
void mock_spi_fail_in_frame(size_t frame, dev_status_t status);

size_t mock_spi_frame_count(void);
const mock_spi_frame_t *mock_spi_frame(size_t i);

/* Порушення протоколу: transfer() при піднятому CS#, повторний select() тощо */
unsigned mock_spi_protocol_errors(void);
int mock_spi_cs_active(void);

#endif /* MOCK_SPI_H */
