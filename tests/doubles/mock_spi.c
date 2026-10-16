/**
  * @file    mock_spi.c
  * @brief   Реалізація mock SPI-шини.
  */
#include "mock_spi.h"

#include <string.h>

typedef struct
{
  uint8_t      miso[MOCK_SPI_FRAME_BYTES];
  size_t       miso_len;
  dev_status_t fail;
} frame_script_t;

static mock_spi_frame_t frames[MOCK_SPI_MAX_FRAMES];
static mock_spi_frame_t overflow_frame;   /* кадри понад ліміт рахуються, але не зберігаються */
static frame_script_t   script[MOCK_SPI_MAX_FRAMES];
static size_t           frame_count;
static int              cs_active;
static unsigned         protocol_errors;

/* Довгі опитування BUSY (стирання — до 400 кадрів) не вміщаються в журнал:
 * такі кадри отримують відповідь останнього кадру сценарію. */
static mock_spi_frame_t *current_frame(void)
{
  return (frame_count < MOCK_SPI_MAX_FRAMES) ? &frames[frame_count] : &overflow_frame;
}

static const frame_script_t *current_script(void)
{
  return &script[(frame_count < MOCK_SPI_MAX_FRAMES) ? frame_count : (MOCK_SPI_MAX_FRAMES - 1U)];
}

static void mock_select(void *ctx)
{
  (void)ctx;
  if (cs_active)
  {
    protocol_errors++;
  }
  cs_active = 1;
  current_frame()->len = 0U;
}

static void mock_deselect(void *ctx)
{
  (void)ctx;
  if (!cs_active)
  {
    protocol_errors++;
    return;
  }
  cs_active = 0;
  frame_count++;
}

static dev_status_t mock_transfer(void *ctx, const uint8_t *tx, uint8_t *rx, size_t len)
{
  (void)ctx;
  if (!cs_active)
  {
    protocol_errors++;
    return DEV_ERR_BUS;
  }

  mock_spi_frame_t *f = current_frame();
  const frame_script_t *s = current_script();

  if (s->fail != DEV_OK)
  {
    return s->fail;
  }
  for (size_t i = 0U; i < len; i++)
  {
    size_t pos = f->len;
    if (pos < MOCK_SPI_FRAME_BYTES)
    {
      f->mosi[pos] = (tx != NULL) ? tx[i] : 0xFFU;
      f->len++;
    }
    if (rx != NULL)
    {
      rx[i] = (pos < s->miso_len) ? s->miso[pos] : 0xFFU;
    }
  }
  return DEV_OK;
}

static const dev_spi_bus_t bus = { NULL, mock_select, mock_deselect, mock_transfer };

void mock_spi_reset(void)
{
  memset(frames, 0, sizeof(frames));
  memset(script, 0, sizeof(script));
  frame_count     = 0U;
  cs_active       = 0;
  protocol_errors = 0U;
}

const dev_spi_bus_t *mock_spi_bus(void)
{
  return &bus;
}

void mock_spi_set_miso(size_t frame, const uint8_t *bytes, size_t len)
{
  if ((frame < MOCK_SPI_MAX_FRAMES) && (len <= MOCK_SPI_FRAME_BYTES))
  {
    memcpy(script[frame].miso, bytes, len);
    script[frame].miso_len = len;
  }
}

void mock_spi_fail_in_frame(size_t frame, dev_status_t status)
{
  if (frame < MOCK_SPI_MAX_FRAMES)
  {
    script[frame].fail = status;
  }
}

size_t mock_spi_frame_count(void)
{
  return frame_count;
}

const mock_spi_frame_t *mock_spi_frame(size_t i)
{
  return (i < MOCK_SPI_MAX_FRAMES) ? &frames[i] : NULL;
}

unsigned mock_spi_protocol_errors(void)
{
  return protocol_errors;
}

int mock_spi_cs_active(void)
{
  return cs_active;
}
