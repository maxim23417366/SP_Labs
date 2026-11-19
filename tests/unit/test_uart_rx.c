/**
  * @file    test_uart_rx.c
  * @brief   ЛР3: прийом USART2 через кільцевий DMA-буфер (firmware/Platform/uart_rx.c)
  *          на моделі DMA1 Stream5 (fake_uart_dma.c).
  *
  *          Дефект, який закриває uart_rx.c: на main fd23599 з 40 команд PING одним
  *          блоком 9 губилися (rx_overrun=14), «PING 12» ставала «PONG seq=1», а
  *          без '\r' команди злипалися («MMARK 2») — бо прийом опитував регістр
  *          USART2 з task_uart(), а цикл стояв довше двох символів.
  */
#include <stdio.h>
#include <string.h>

#include "unity.h"
#include "platform.h"
#include "uart_rx.h"
#include "fake_uart_dma.h"

#define HALF  (UART_RX_BUF / 2U)

void setUp(void)
{
  fake_uart_dma_reset();
  UartRx_Start();
}

void tearDown(void)
{
}

/* Прочитати все прийняте в buf; повертає кількість, LOST рахує окремо */
static size_t drain(char *buf, size_t cap, unsigned *lost)
{
  size_t n = 0U;
  int c;

  *lost = 0U;
  while ((c = UartRx_Get()) != PLATFORM_UART_NONE)
  {
    if (c == PLATFORM_UART_LOST)
    {
      (*lost)++;
    }
    else if (n < cap)
    {
      buf[n++] = (char)c;
    }
  }
  return n;
}

/* Детермінований потік: байт i = (i * 7 + 3) & 0xFF */
static uint8_t pattern(uint32_t i)
{
  return (uint8_t)((i * 7U) + 3U);
}

static void send_pattern(uint32_t from, uint32_t len)
{
  for (uint32_t i = 0U; i < len; i++)
  {
    uint8_t b = pattern(from + i);
    fake_uart_dma_rx(&b, 1U);
  }
}

static void expect_pattern(uint32_t from, uint32_t len)
{
  for (uint32_t i = 0U; i < len; i++)
  {
    int c = UartRx_Get();
    TEST_ASSERT_EQUAL_INT_MESSAGE((int)pattern(from + i), c, "байт потоку");
  }
}

/* ============================================================================ */

static void test_start_configures_dma_ring(void)
{
  TEST_ASSERT_EQUAL_UINT32(1U, fake_uart_dma.starts);
  TEST_ASSERT_EQUAL_UINT32(UART_RX_BUF, fake_uart_dma.n);
  TEST_ASSERT_EQUAL_UINT32(UART_RX_BUF, fake_uart_dma.ndtr);
  TEST_ASSERT_EQUAL_INT(PLATFORM_UART_NONE, UartRx_Get());
  TEST_ASSERT_EQUAL_UINT32(0U, UartRx_Lost());
  TEST_ASSERT_EQUAL_UINT32(0U, UartRx_Peak());
}

/* 40 команд одним блоком (як у дефекті), цикл не читав увесь цей час: нічого не загублено */
static void test_block_of_40_pings_while_loop_busy(void)
{
  char sent[512];
  char got[512];
  size_t len = 0U;
  unsigned lost;

  for (unsigned i = 1U; i <= 40U; i++)
  {
    len += (size_t)snprintf(&sent[len], sizeof(sent) - len, "PING %u\r", i);
  }
  fake_uart_dma_rx_str(sent);                  /* 332 байти ≈ 29 мс прийому без читання */

  TEST_ASSERT_EQUAL_size_t(len, drain(got, sizeof(got), &lost));
  TEST_ASSERT_EQUAL_UINT(0U, lost);
  TEST_ASSERT_EQUAL_MEMORY(sent, got, len);
  TEST_ASSERT_EQUAL_UINT32(0U, UartRx_Lost());
  TEST_ASSERT_EQUAL_UINT32((uint32_t)len, UartRx_Peak());
}

/* Багато обертів кільця порціями різного розміру: порядок байтів збережено */
static void test_many_laps_keep_order(void)
{
  static const uint32_t chunk[] = { 1U, 300U, HALF - 1U, HALF, HALF + 1U, 7U, UART_RX_BUF - 100U };
  uint32_t at = 0U;

  for (unsigned lap = 0U; lap < 5U; lap++)
  {
    for (unsigned k = 0U; k < sizeof(chunk) / sizeof(chunk[0]); k++)
    {
      send_pattern(at, chunk[k]);
      expect_pattern(at, chunk[k]);
      at += chunk[k];
      TEST_ASSERT_EQUAL_INT(PLATFORM_UART_NONE, UartRx_Get());
    }
  }
  TEST_ASSERT_EQUAL_UINT32(0U, UartRx_Lost());
  TEST_ASSERT_TRUE(fake_uart_dma.irqs >= (at / HALF) - 1U);
}

/* Переривання замасковані (DHT11 ~5 мс = ~58 байтів, лінія кадру OV7670): DMA пише далі,
 * HT/TC обробляються пізніше — байти коректні і до, і після обробки */
static void test_masked_irq_across_half_and_wrap(void)
{
  uint32_t at = 0U;

  send_pattern(at, HALF - 30U);
  expect_pattern(at, HALF - 30U);
  at += HALF - 30U;

  fake_uart_dma.irq_masked = 1;
  send_pattern(at, 60U);                       /* перетнули половину, обробник ще не був */
  expect_pattern(at, 40U);
  at += 40U;
  fake_uart_dma_unmask();
  expect_pattern(at, 20U);
  at += 20U;

  /* те саме на кінці кільця (TC) */
  send_pattern(at, HALF - 60U - 10U);
  expect_pattern(at, HALF - 70U);
  at += HALF - 70U;
  fake_uart_dma.irq_masked = 1;
  send_pattern(at, 64U);
  expect_pattern(at, 64U);
  at += 64U;
  fake_uart_dma_unmask();
  send_pattern(at, 5U);
  expect_pattern(at, 5U);
  TEST_ASSERT_EQUAL_INT(PLATFORM_UART_NONE, UartRx_Get());
  TEST_ASSERT_EQUAL_UINT32(0U, UartRx_Lost());
}

/* HT обробляється між читанням лічильника половин і NDTR: застаріле h дає той самий W */
static void test_irq_between_counter_and_ndtr_reads(void)
{
  uint32_t at = 0U;

  send_pattern(at, HALF - 2U);
  expect_pattern(at, HALF - 2U);
  at += HALF - 2U;
  fake_uart_dma.irq_masked = 1;
  send_pattern(at, 5U);                        /* HT виставлено, обробник чекає */
  fake_uart_dma.irq_masked = 0;
  fake_uart_dma.irq_on_ndtr = 1;               /* і спрацює посеред UartRx_Get() */
  expect_pattern(at, 5U);
  TEST_ASSERT_EQUAL_INT(PLATFORM_UART_NONE, UartRx_Get());
  TEST_ASSERT_EQUAL_UINT32(0U, UartRx_Lost());
}

/* Кільце майже повне (запас 16 байтів) — ще без втрат */
static void test_almost_full_ring_is_not_loss(void)
{
  send_pattern(0U, UART_RX_BUF - 16U);
  expect_pattern(0U, UART_RX_BUF - 16U);
  TEST_ASSERT_EQUAL_UINT32(0U, UartRx_Lost());
  TEST_ASSERT_EQUAL_UINT32(UART_RX_BUF - 16U, UartRx_Peak());
}

/* Переповнення: цикл не читав довше ~178 мс під потоком — один LOST на місці втрати,
 * старі байти відкинуто, нові приймаються далі */
static void test_overflow_reports_lost_once_and_recovers(void)
{
  send_pattern(0U, 10U);
  expect_pattern(0U, 10U);
  send_pattern(10U, UART_RX_BUF + 100U);

  TEST_ASSERT_EQUAL_INT(PLATFORM_UART_LOST, UartRx_Get());
  TEST_ASSERT_EQUAL_INT(PLATFORM_UART_NONE, UartRx_Get());
  TEST_ASSERT_EQUAL_UINT32(1U, UartRx_Lost());
  TEST_ASSERT_EQUAL_UINT32(UART_RX_BUF, UartRx_Peak());

  send_pattern(5000U, 50U);
  expect_pattern(5000U, 50U);
  TEST_ASSERT_EQUAL_INT(PLATFORM_UART_NONE, UartRx_Get());
  TEST_ASSERT_EQUAL_UINT32(1U, UartRx_Lost());
}

/* Повторний старт (скидання) обнуляє стан */
static void test_restart_resets_state(void)
{
  send_pattern(0U, UART_RX_BUF);
  TEST_ASSERT_EQUAL_INT(PLATFORM_UART_LOST, UartRx_Get());
  UartRx_Start();
  TEST_ASSERT_EQUAL_UINT32(2U, fake_uart_dma.starts);
  TEST_ASSERT_EQUAL_UINT32(0U, UartRx_Lost());
  TEST_ASSERT_EQUAL_UINT32(0U, UartRx_Peak());
  TEST_ASSERT_EQUAL_INT(PLATFORM_UART_NONE, UartRx_Get());
  send_pattern(7U, 3U);
  expect_pattern(7U, 3U);
}

int main(void)
{
  UNITY_BEGIN();
  RUN_TEST(test_start_configures_dma_ring);
  RUN_TEST(test_block_of_40_pings_while_loop_busy);
  RUN_TEST(test_many_laps_keep_order);
  RUN_TEST(test_masked_irq_across_half_and_wrap);
  RUN_TEST(test_irq_between_counter_and_ndtr_reads);
  RUN_TEST(test_almost_full_ring_is_not_loss);
  RUN_TEST(test_overflow_reports_lost_once_and_recovers);
  RUN_TEST(test_restart_resets_state);
  return UNITY_END();
}
