/**
  ******************************************************************************
  * @file    cam_test.h
  * @brief   ЛР3 (I07): команда запуску камери на платі — CAM TEST.
  *
  *          CAM TEST ініціалізує OV7670 по SCCB (PID/VER, скидання, CLKRC,
  *          таблиця QQVGA RGB565) і знімає один кадр через
  *          Platform_CameraCapture() у статичний буфер CAMERA_FRAME_BYTES.
  *          Друкує (docs/camera.md §1, інструкція — §7):
  *            CAM,init,status=..,pid=0x..,ver=0x..,clkrc=0x..,failed_reg=0x..
  *            CAM,result,status=..,bytes=..,lines=..,short=..,vglitch=..,
  *                       capture_us=..,crc=0x........
  *          і подієві мітки EVT capture_start / capture_done (metrics-spec §3.3).
  *          Тривалість захоплення — у STATS рядком STAT name=camera.
  *
  *          I09: CAM LOAD <frame_id> — кадр із W25Q64 назад у буфер (CAM,load,...,
  *          event=<event_seq>); CAM EXPORT передає його з id = ID події.
  *
  *          I08: CAM PATTERN — синтетичний кадр у той самий буфер (без камери);
  *          CAM EXPORT — передача останнього кадру на ПК (frame_export.c,
  *          tools/frame_decode.py). Поки йде експорт, CAM TEST / CAM PATTERN
  *          відповідають CAM,busy і буфер не змінюють.
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
#include "platform.h"

/* true — рядок оброблено (CAM TEST); false — це не команда камери */
bool CamTest_LineCommand(const char *line);

/* ЛР3: налаштувати камеру при старті (App_Init), щоб AEC/AWB встигли зійтися до
 * першої тривоги. Друкує CAM,init; якщо камери немає — повтор при першому кадрі. */
void CamTest_Init(void);

/* true — камера налаштована щонайменше TBD_CAMERA_AEC_SETTLE_MS тому: AEC/AWB зійшлися.
 * Шлях тривоги не чекає сходження, а лише позначає кадр (CAM,photo,...,aec=) */
bool CamTest_AecSettled(void);

/* Тривалість захоплення в тактах DWT (STAT name=camera) */
const perf_stat_t *CamTest_Stat(void);
void CamTest_ResetStats(void);

/* I09: тривалість запису кадру у W25Q64 (STAT name=flash_frame; оновлює camera.c) */
perf_stat_t *CamTest_StoreStat(void);

/* Останній знятий кадр (bytes — скільки байтів прийнято) */
const uint8_t *CamTest_Frame(size_t *bytes);

/* I08: синтетичний кадр RGB565 (смуги + градієнт), як tools/frame_decode.py::test_pattern() */
void CamTest_FillTestPattern(uint8_t *buf, uint16_t width, uint16_t height);

/* I09: один кадр у спільний буфер (init камери при першому виклику, EVT capture_*,
 * STAT camera). true — кадр повний; *status — статус Platform_CameraCapture(), а якщо
 * камера не відповіла по SCCB — PLATFORM_CAM_ERR_ARG (причина в рядку CAM,init). */
bool CamTest_Capture(platform_cam_status_t *status, platform_cam_result_t *res);

/* I09: кадр у буфері належить події event_id — CAM EXPORT передасть id=event_id */
void CamTest_SetFrameEvent(uint32_t event_id);

/* true — буфер кадру зайнятий експортом (I08), знімати в нього не можна */
bool CamTest_FrameLocked(void);

const char *CamTest_StatusName(platform_cam_status_t s);

/* Повторна ініціалізація камери при наступному CAM TEST (тести, перепідключення) */
void CamTest_Reset(void);

#endif /* CAM_TEST_H */
