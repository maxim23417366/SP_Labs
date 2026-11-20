/**
  * @file    fake_uart_dma.c
  * @brief   Модель DMA1 Stream5 / USART2_RX для host-тестів uart_rx.c (fake_uart_dma.h).
  */
#include "fake_uart_dma.h"

#include <string.h>

fake_uart_dma_t fake_uart_dma;

void fake_uart_dma_reset(void)
{
  memset(&fake_uart_dma, 0, sizeof(fake_uart_dma));
}

void fake_uart_dma_start(uint8_t *buf, uint32_t n)
{
  fake_uart_dma.buf  = buf;
  fake_uart_dma.n    = n;
  fake_uart_dma.ndtr = n;
  fake_uart_dma.ht   = 0U;
  fake_uart_dma.tc   = 0U;
  fake_uart_dma.starts++;
}

uint32_t fake_uart_dma_ndtr(void)
{
  if (fake_uart_dma.irq_on_ndtr != 0)
  {
    fake_uart_dma.irq_on_ndtr = 0;
    fake_uart_dma_irq();
  }
  return fake_uart_dma.ndtr;
}

uint32_t fake_uart_dma_take_halves(void)
{
  uint32_t n = fake_uart_dma.ht + fake_uart_dma.tc;

  fake_uart_dma.ht = 0U;
  fake_uart_dma.tc = 0U;
  return n;
}

static void raise_irq(void)
{
  if (fake_uart_dma.irq_masked == 0)
  {
    fake_uart_dma.irqs++;
    fake_uart_dma_irq();
  }
}

void fake_uart_dma_rx(const uint8_t *data, size_t len)
{
  for (size_t i = 0U; i < len; i++)
  {
    fake_uart_dma.buf[fake_uart_dma.n - fake_uart_dma.ndtr] = data[i];
    fake_uart_dma.ndtr--;
    if (fake_uart_dma.ndtr == (fake_uart_dma.n / 2U))
    {
      fake_uart_dma.ht = 1U;   /* прапорець — один біт: друге HT до обробника губиться, як у залізі */
      raise_irq();
    }
    else if (fake_uart_dma.ndtr == 0U)
    {
      fake_uart_dma.ndtr = fake_uart_dma.n;   /* кільцевий режим: NDTR перезавантажується */
      fake_uart_dma.tc = 1U;
      raise_irq();
    }
  }
}

void fake_uart_dma_rx_str(const char *s)
{
  fake_uart_dma_rx((const uint8_t *)s, strlen(s));
}

void fake_uart_dma_unmask(void)
{
  fake_uart_dma.irq_masked = 0;
  if ((fake_uart_dma.ht | fake_uart_dma.tc) != 0U)
  {
    fake_uart_dma.irqs++;
    fake_uart_dma_irq();
  }
}
