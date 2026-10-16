/**
  ******************************************************************************
  * @file    platform.c
  * @brief   Час і перевірка периферії МК для App/.
  ******************************************************************************
  */
#include "board.h"
#include "periph_check.h"
#include "platform.h"

void Platform_DelayMs(uint32_t ms)
{
  HAL_Delay(ms);
}

uint32_t Platform_Millis(void)
{
  return HAL_GetTick();
}

void Platform_BringUp(void)
{
  PeriphCheck_RunAll();
}

void Platform_Poll(void)
{
  PeriphCheck_Loop();
}
