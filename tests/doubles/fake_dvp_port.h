/**
  * @file    fake_dvp_port.h
  * @brief   Підміна camera_dvp_port.h на ПК: сигнали OV7670 генерує модель
  *          fake_dvp.c, а не GPIO. Підключається до firmware/Platform/camera_dvp.c
  *          макросом PLATFORM_DVP_FAKE (tests/Makefile).
  *
  *          Кожне читання «порту» — один крок модельного часу (і кілька тактів
  *          fake DWT, щоб працювали тайм-аути camera_dvp.c).
  */
#ifndef FAKE_DVP_PORT_H
#define FAKE_DVP_PORT_H

#include <stdint.h>

uint32_t fake_dvp_read(void);    /* біти як у GPIOC->IDR: D0..D7, VSYNC = 8, HREF = 10 */
uint32_t fake_dvp_pclk(void);
uint32_t fake_dvp_irq_save(void);
void     fake_dvp_irq_restore(uint32_t primask);

#define dvp_read()            fake_dvp_read()
#define dvp_pclk()            fake_dvp_pclk()
#define dvp_irq_save()        fake_dvp_irq_save()
#define dvp_irq_restore(p)    fake_dvp_irq_restore(p)

#define DVP_DATA(w)   ((uint8_t)((w) & 0xFFU))
#define DVP_VSYNC(w)  (((w) >> 8) & 1U)
#define DVP_HREF(w)   (((w) >> 10) & 1U)

#endif /* FAKE_DVP_PORT_H */
