/**
  ******************************************************************************
  * @file    camera_dvp_port.h
  * @brief   Доступ до сигналів DVP OV7670 на STM32F411 (піни — docs/pinout.md):
  *            D0..D7 = PC0..PC7, VSYNC = PC8, HREF = PC10 → одне читання GPIOC->IDR;
  *            PCLK   = PA10.
  *
  *          Включається лише з camera_dvp.c. У хостових тестах замість нього
  *          береться tests/doubles/fake_dvp_port.h (макрос PLATFORM_DVP_FAKE).
  ******************************************************************************
  */
#ifndef CAMERA_DVP_PORT_H
#define CAMERA_DVP_PORT_H

#include "main.h"

static inline uint32_t dvp_read(void)
{
  return GPIOC->IDR;
}

static inline uint32_t dvp_pclk(void)
{
  return (GPIOA->IDR >> 10) & 1U;
}

#define DVP_DATA(w)   ((uint8_t)((w) & 0xFFU))
#define DVP_VSYNC(w)  (((w) >> 8) & 1U)
#define DVP_HREF(w)   (((w) >> 10) & 1U)

/* Маскування переривань на час однієї лінії: SysTick/EXTI посеред лінії
 * зсунули б вибірку на кілька фронтів PCLK. Повертає попередній PRIMASK. */
static inline uint32_t dvp_irq_save(void)
{
  uint32_t primask = __get_PRIMASK();
  __disable_irq();
  return primask;
}

static inline void dvp_irq_restore(uint32_t primask)
{
  if (primask == 0U)
  {
    __enable_irq();
  }
}

#endif /* CAMERA_DVP_PORT_H */
