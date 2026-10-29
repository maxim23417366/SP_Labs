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

static int loop_traffic = 0;

void Platform_BringUp(void)
{
  Platform_IoInit();
  PeriphCheck_RunAll();
}

void Platform_SetLoopTraffic(int on)
{
  loop_traffic = on;
}

void Platform_Poll(void)
{
  /* Трафік ЛР1 кожні 500 мс вмикається командою «l»: він зручний для запису
   * аналізатором, але інакше заважав би читати вивід команд стенда. */
  if (loop_traffic)
  {
    PeriphCheck_Loop();
  }
}
