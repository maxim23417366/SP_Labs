/**
  * @file    fake_uart_dma.h
  * @brief   Модель DMA1 Stream5 у кільцевому режимі для Platform/uart_rx.c: байти з
  *          «лінії» пишуться в буфер, NDTR спадає, на половині й наприкінці кільця
  *          виставляються HT / TC і викликається переривання — одразу або, якщо
  *          переривання «замасковані» (__disable_irq у DHT11 / камері), пізніше.
  */
#ifndef FAKE_UART_DMA_H
#define FAKE_UART_DMA_H

#include <stddef.h>
#include <stdint.h>

typedef struct
{
  uint8_t  *buf;
  uint32_t  n;
  uint32_t  ndtr;
  uint32_t  ht;              /* прапорці, ще не забрані обробником */
  uint32_t  tc;
  int       irq_masked;      /* 1 — переривання DMA чекає fake_uart_dma_unmask() */
  int       irq_on_ndtr;     /* 1 — наступне читання NDTR спершу запускає обробник
                              * (HT/TC прийшло між читанням лічильника і NDTR) */
  uint32_t  starts;
  uint32_t  irqs;
} fake_uart_dma_t;

extern fake_uart_dma_t fake_uart_dma;

void fake_uart_dma_reset(void);
void fake_uart_dma_rx(const uint8_t *data, size_t len);   /* байти, прийняті USART2 */
void fake_uart_dma_rx_str(const char *s);
void fake_uart_dma_unmask(void);                          /* __enable_irq(): обробити HT/TC */
void fake_uart_dma_irq(void);                             /* обробник з uart_rx.c */

#endif /* FAKE_UART_DMA_H */
