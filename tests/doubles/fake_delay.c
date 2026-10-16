/**
  * @file    fake_delay.c
  */
#include "fake_delay.h"

uint32_t fake_delay_total_ms;
uint32_t fake_delay_calls;

void fake_delay_reset(void)
{
  fake_delay_total_ms = 0U;
  fake_delay_calls    = 0U;
}

void fake_delay_ms(uint32_t ms)
{
  fake_delay_total_ms += ms;
  fake_delay_calls++;
}
