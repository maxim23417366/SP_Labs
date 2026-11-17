/**
  ******************************************************************************
  * @file    cam_test.h
  * @brief   ЛР3 (I07): команда запуску камери на платі — CAM TEST.
  *
  *          CAM TEST ініціалізує OV7670 по SCCB (PID/VER, скидання, CLKRC,
  *          таблиця QQVGA RGB565) і знімає один кадр через
  *          Platform_CameraCapture() у статичний буфер CAMERA_FRAME_BYTES.
  *          Друкує (docs/camera.md §5):
  *            CAM,init,status=..,pid=0x..,ver=0x..,clkrc=0x..,failed_reg=0x..
  *            CAM,result,status=..,bytes=..,lines=..,short=..,vglitch=..,
  *                       capture_us=..,crc=0x........
  *          і подієві мітки EVT capture_start / capture_done (metrics-spec §3.3).
  *          Тривалість захоплення — у STATS рядком STAT name=camera.
  *
  *          Команда блокує цикл на час кадру (до ~1 с при CLKRC = 0x0F) — це
  *          стендова перевірка для H14. Задача камери в Super Loop і передача
  *          кадру на ПК — картки I08–I09; вони використовують той самий буфер.
  ******************************************************************************
  */
#ifndef CAM_TEST_H
#define CAM_TEST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "perf.h"

/* true — рядок оброблено (CAM TEST); false — це не команда камери */
bool CamTest_LineCommand(const char *line);

/* Тривалість захоплення в тактах DWT (STAT name=camera) */
const perf_stat_t *CamTest_Stat(void);
void CamTest_ResetStats(void);

/* Останній знятий кадр (bytes — скільки байтів прийнято) */
const uint8_t *CamTest_Frame(size_t *bytes);

/* Повторна ініціалізація камери при наступному CAM TEST (тести, перепідключення) */
void CamTest_Reset(void);

#endif /* CAM_TEST_H */
