/**
  ******************************************************************************
  * @file    uart_rx_port.h
  * @brief   ЛР3: DMA1 Stream5 / Channel 4 ← USART2_RX на STM32F411 (RM0383 §9.3.3,
  *          табл. 27). Включається лише з uart_rx.c; на ПК замість нього
  *          tests/doubles/fake_uart_rx_port.h (макрос PLATFORM_UART_FAKE).
  *
  *          DMA у firmware.ioc не налаштовано, тому регістри пишуться тут, а
  *          DMA1_Stream5_IRQHandler визначає uart_rx.c (ім'я — UART_RX_DMA_IRQHandler). Якщо колись додати
  *          USART2_RX DMA у CubeMX, згенерований обробник і MX_DMA_Init() конфліктуватимуть
  *          з цим файлом — лінкер повідомить про подвійний DMA1_Stream5_IRQHandler.
  ******************************************************************************
  */
#ifndef UART_RX_PORT_H
#define UART_RX_PORT_H

#include "main.h"

/* Пріоритет переривання DMA: найнижчий. Обробник лише рахує половини кільця, а
 * запізнення до UART_RX_BUF/2 байтів (89 мс) не шкодить (uart_rx.c). */
#define UART_RX_DMA_IRQ_PRIO  15U

static inline void uart_rx_hw_start(uint8_t *buf, uint32_t n)
{
  DMA_Stream_TypeDef *s = DMA1_Stream5;

  __HAL_RCC_DMA1_CLK_ENABLE();

  USART2->CR3 &= ~USART_CR3_DMAR;
  s->CR &= ~DMA_SxCR_EN;
  while ((s->CR & DMA_SxCR_EN) != 0U)
  {
  }
  /* прапорці Stream5 — у HIFCR */
  DMA1->HIFCR = DMA_HIFCR_CTCIF5 | DMA_HIFCR_CHTIF5 | DMA_HIFCR_CTEIF5 | DMA_HIFCR_CDMEIF5 |
                DMA_HIFCR_CFEIF5;

  s->PAR  = (uint32_t)&USART2->DR;
  s->M0AR = (uint32_t)buf;
  s->NDTR = n;
  s->FCR  = 0U;                                /* direct mode, без FIFO */
  s->CR   = (4U << DMA_SxCR_CHSEL_Pos)         /* Channel 4 = USART2_RX */
          | DMA_SxCR_PL_1                      /* пріоритет high */
          | DMA_SxCR_MINC | DMA_SxCR_CIRC      /* кільце в пам'яті, DIR = 00 (P→M), 8 біт */
          | DMA_SxCR_HTIE | DMA_SxCR_TCIE;

  /* Байт, що міг залишитися в DR до старту (і ORE від нього), — скинути: SR, потім DR */
  (void)USART2->SR;
  (void)USART2->DR;

  HAL_NVIC_SetPriority(DMA1_Stream5_IRQn, UART_RX_DMA_IRQ_PRIO, 0U);
  HAL_NVIC_EnableIRQ(DMA1_Stream5_IRQn);

  s->CR |= DMA_SxCR_EN;
  USART2->CR3 |= USART_CR3_DMAR;           /* USART2 просить DMA на кожен прийнятий байт */
}

/* Обробник переривання DMA, який визначає uart_rx.c */
#define UART_RX_DMA_IRQHandler  DMA1_Stream5_IRQHandler

/* З ISR: скільки половин кільця відмітив DMA (HT, TC), прапорці знято. Помилки
 * потоку (TE, DME, FE) лише знімаються: у direct mode з адресою DR вони не виникають. */
static inline uint32_t uart_rx_hw_take_halves(void)
{
  uint32_t isr = DMA1->HISR;
  uint32_t n   = 0U;

  if ((isr & DMA_HISR_HTIF5) != 0U)
  {
    n++;
  }
  if ((isr & DMA_HISR_TCIF5) != 0U)
  {
    n++;
  }
  DMA1->HIFCR = DMA_HIFCR_CTCIF5 | DMA_HIFCR_CHTIF5 | DMA_HIFCR_CTEIF5 | DMA_HIFCR_CDMEIF5 |
                DMA_HIFCR_CFEIF5;
  return n;
}

static inline uint32_t uart_rx_hw_ndtr(void)
{
  return DMA1_Stream5->NDTR;
}

#endif /* UART_RX_PORT_H */
