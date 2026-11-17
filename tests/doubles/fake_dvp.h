/**
  * @file    fake_dvp.h
  * @brief   Модель сигналів DVP OV7670 для тесту camera_dvp.c на ПК.
  *
  *          Кадр (повторюється по колу):
  *            VSYNC = 1 протягом vsync_steps → пауза gap_steps →
  *            height ліній: гасіння blank_steps (HREF = 0), потім line_bytes байтів
  *            з HREF = 1; кожен байт — half_period кроків PCLK = 0, half_period = 1 →
  *            хвіст tail_steps.
  *          PCLK тактує безперервно. Байт x лінії y = fake_dvp_byte(y, x).
  *          Крок моделі = одне читання порту; кожен крок додає cycles_per_read
  *          тактів fake DWT.
  */
#ifndef FAKE_DVP_H
#define FAKE_DVP_H

#include <stdint.h>

typedef struct
{
  uint32_t line_bytes;
  uint32_t height;
  uint32_t half_period;
  uint32_t vsync_steps;
  uint32_t gap_steps;
  uint32_t blank_steps;
  uint32_t tail_steps;
  uint32_t start_step;        /* з якого кроку кадру почати (захоплення посеред кадру) */
  uint32_t cycles_per_read;

  /* несправності */
  int      no_vsync;          /* VSYNC завжди 0 */
  int32_t  short_line;        /* номер лінії, коротшої на 3 байти (-1 — немає) */
  uint32_t extra_bytes;       /* кожна лінія довша на стільки байтів */
  int32_t  dead_pclk_line;    /* з цієї лінії PCLK завмирає в 0 (-1 — ні) */
  int32_t  no_href_after;     /* після цієї кількості ліній HREF більше не піднімається (-1) */
  uint32_t glitch_at;         /* завада: VSYNC = 1 з кроку кадру glitch_at ... */
  uint32_t glitch_steps;      /* ... протягом glitch_steps кроків (0 — завади немає) */

  /* спостереження */
  uint32_t step;
  uint32_t irq_saves;
  uint32_t irq_restores;
  uint32_t irq_depth_max;
  uint32_t irq_depth;
} fake_dvp_t;

extern fake_dvp_t fake_dvp;

void    fake_dvp_reset(uint32_t line_bytes, uint32_t height);
uint8_t fake_dvp_byte(uint32_t y, uint32_t x);

#endif /* FAKE_DVP_H */
