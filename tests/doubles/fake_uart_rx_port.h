/**
  * @file    fake_uart_rx_port.h
  * @brief   Підміна uart_rx_port.h на ПК: DMA1 Stream5 (USART2_RX) моделює
  *          fake_uart_dma.c. Підключається до firmware/Platform/uart_rx.c макросом
  *          PLATFORM_UART_FAKE (tests/Makefile).
  */
#ifndef FAKE_UART_RX_PORT_H
#define FAKE_UART_RX_PORT_H

#include <stdint.h>

void     fake_uart_dma_start(uint8_t *buf, uint32_t n);
uint32_t fake_uart_dma_ndtr(void);
uint32_t fake_uart_dma_take_halves(void);

#define uart_rx_hw_start(b, n)     fake_uart_dma_start((b), (n))
#define uart_rx_hw_ndtr()          fake_uart_dma_ndtr()
#define uart_rx_hw_take_halves()   fake_uart_dma_take_halves()
#define UART_RX_DMA_IRQHandler     fake_uart_dma_irq

#endif /* FAKE_UART_RX_PORT_H */
