/**
  ******************************************************************************
  * @file    retarget.c
  * @brief   printf → USART2 (ST-LINK VCP).
  *
  *          Згенерований CubeMX syscalls.c передає кожен символ printf у
  *          __io_putchar(). Без цієї функції printf ішов би в нікуди.
  ******************************************************************************
  */
#include "board.h"

int __io_putchar(int ch);

int __io_putchar(int ch)
{
  uint8_t c = (uint8_t)ch;

  HAL_UART_Transmit(BOARD_LOG_UART, &c, 1U, BOARD_UART_TIMEOUT_MS);
  return ch;
}
