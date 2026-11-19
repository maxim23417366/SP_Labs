/**
  ******************************************************************************
  * @file    uart_rx.c
  * @brief   ЛР3: прийом USART2 через кільцевий DMA-буфер (див. uart_rx.h).
  *
  *          Скільки байтів записав DMA (W, монотонний лічильник):
  *            h — скільки половин кільця відмітило переривання DMA (HT і TC);
  *            pos = UART_RX_BUF - NDTR — поточна позиція запису в кільці;
  *            W = h * HALF + ((pos - h * HALF) mod UART_RX_BUF).
  *          Формула правильна, поки переривання запізнюється менше ніж на
  *          UART_RX_BUF байтів (у прошивці — щонайбільше ~5 мс = 58 байтів), тож
  *          узгоджувати h і NDTR повторним читанням не треба.
  *          Програма читає з позиції R; W - R > UART_RX_BUF - UART_RX_GUARD означає,
  *          що DMA вже переписав (або от-от перепише) непрочитане — R стрибає на W,
  *          а Get() повертає PLATFORM_UART_LOST.
  ******************************************************************************
  */
#include "platform.h"
#include "uart_rx.h"

#ifdef PLATFORM_UART_FAKE
#include "fake_uart_rx_port.h"
#else
#include "uart_rx_port.h"
#endif

#define HALF          (UART_RX_BUF / 2U)
#define MASK          (UART_RX_BUF - 1U)
/* Запас на байти, що DMA встигне записати, поки Get() читає один символ */
#define UART_RX_GUARD 16U

void UART_RX_DMA_IRQHandler(void);

#if (UART_RX_BUF & MASK) != 0U
#error "UART_RX_BUF має бути степенем двійки"
#endif

static uint8_t           s_buf[UART_RX_BUF];
static volatile uint32_t s_halves;     /* пише лише ISR DMA */
static uint32_t          s_read;       /* R: скільки байтів забрала програма */
static uint32_t          s_lost;
static uint32_t          s_peak;

/* Переривання DMA (HT / TC): ще половина кільця записана */
void UART_RX_DMA_IRQHandler(void)
{
  s_halves += uart_rx_hw_take_halves();
}

void UartRx_Start(void)
{
  s_halves = 0U;
  s_read   = 0U;
  s_lost   = 0U;
  s_peak   = 0U;
  uart_rx_hw_start(s_buf, UART_RX_BUF);
}

static uint32_t written(void)
{
  /* Порядок важливий: спершу h, потім NDTR. Обробник HT/TC може лише відставати від
   * NDTR (запізнюватися), і формула це витримує: застаріле h дає той самий W. Навпаки
   * (NDTR раніше за h) — h могло б випередити позицію, і W «перескочив» би кільце. */
  uint32_t h    = s_halves;
  uint32_t ndtr = uart_rx_hw_ndtr();
  uint32_t base = h * HALF;

  return base + (((UART_RX_BUF - ndtr) - base) & MASK);
}

int UartRx_Get(void)
{
  uint32_t w = written();
  uint32_t backlog = w - s_read;

  if (backlog == 0U)
  {
    return PLATFORM_UART_NONE;
  }
  if (backlog > (UART_RX_BUF - UART_RX_GUARD))
  {
    s_lost++;
    s_peak = UART_RX_BUF;
    s_read = w;          /* усе непрочитане ненадійне — відкидаємо */
    return PLATFORM_UART_LOST;
  }
  if (backlog > s_peak)
  {
    s_peak = backlog;
  }
  return (int)s_buf[(s_read++) & MASK];
}

uint32_t UartRx_Lost(void)
{
  return s_lost;
}

uint32_t UartRx_Peak(void)
{
  return s_peak;
}
