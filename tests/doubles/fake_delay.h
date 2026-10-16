/**
  * @file    fake_delay.h
  * @brief   Фіктивний час: замість HAL_Delay() лише рахує, скільки драйвер «чекав».
  */
#ifndef FAKE_DELAY_H
#define FAKE_DELAY_H

#include <stdint.h>

extern uint32_t fake_delay_total_ms;
extern uint32_t fake_delay_calls;

void fake_delay_reset(void);
void fake_delay_ms(uint32_t ms);

#endif /* FAKE_DELAY_H */
