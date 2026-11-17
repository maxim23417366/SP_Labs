/**
  ******************************************************************************
  * @file    camera_dvp.c
  * @brief   ЛР3 (I07): програмне захоплення одного кадру OV7670 через DVP GPIO.
  *
  *          STM32F411 не має DCMI, тому кожен байт читається процесором за
  *          наростаючим фронтом PCLK. Процесор зайнятий увесь кадр (плюс
  *          очікування його початку) — у Super Loop це найдовша блокувальна
  *          операція, яку вимірюють S3 і H14 (docs/camera.md).
  *
  *          Припущення про сигнали (за datasheet OV7670, «Horizontal Timing» /
  *          «VGA Frame Timing»; підтверджуються записом PulseView у H14):
  *            - VSYNC — імпульс активного рівня 1 на початку кадру (COM10[1] = 0);
  *            - HREF  = 1 на час валідних байтів лінії;
  *            - дані стабільні на наростаючому фронті PCLK (COM10[4] = 0);
  *            - PCLK тактує і в гасінні (COM10[5] = 0), тож «мертвий» PCLK —
  *              це несправність, а не пауза.
  ******************************************************************************
  */
#include "platform.h"

#ifdef PLATFORM_DVP_FAKE
#include "fake_dvp_port.h"
#else
#include "camera_dvp_port.h"
#endif

static uint32_t us_to_cycles(uint32_t us)
{
  return (uint32_t)(((uint64_t)us * Platform_CpuHz()) / 1000000U);
}

static int wait_vsync(uint32_t level, uint32_t timeout_cyc)
{
  uint32_t t0 = Platform_Cycles();

  while (DVP_VSYNC(dvp_read()) != level)
  {
    if ((uint32_t)(Platform_Cycles() - t0) >= timeout_cyc)
    {
      return 0;
    }
  }
  return 1;
}

static int wait_href_high(uint32_t timeout_cyc)
{
  uint32_t t0 = Platform_Cycles();

  while (DVP_HREF(dvp_read()) == 0U)
  {
    if ((uint32_t)(Platform_Cycles() - t0) >= timeout_cyc)
    {
      return 0;
    }
  }
  return 1;
}

/* Початок кадру: VSYNC 0 → 1 → 0, причому імпульс 1 має тривати не менше
 * min_cyc. Коротший імпульс — завада на лінії (на стенді лічильник VSYNC
 * бачив до 4092 «кадрів»/с під час роботи руками біля плати, лог 15 ЛР3):
 * без фільтра захоплення почалось би посеред кадру і зшило б два кадри.
 * Загальна межа — 3 × timeout, щоб безперервні завади не тримали цикл вічно. */
static int wait_frame_start(uint32_t min_cyc, uint32_t timeout_cyc, uint16_t *glitches)
{
  uint32_t t0 = Platform_Cycles();
  uint64_t limit = (uint64_t)timeout_cyc * 3U;

  if (!wait_vsync(0U, timeout_cyc))
  {
    return 0;
  }
  for (;;)
  {
    uint32_t t_rise;

    if (!wait_vsync(1U, timeout_cyc))
    {
      return 0;
    }
    t_rise = Platform_Cycles();
    if (!wait_vsync(0U, timeout_cyc))
    {
      return 0;
    }
    if ((uint32_t)(Platform_Cycles() - t_rise) >= min_cyc)
    {
      return 1;
    }
    if (*glitches != UINT16_MAX)
    {
      (*glitches)++;
    }
    if ((uint32_t)(Platform_Cycles() - t0) >= limit)
    {
      return 0;
    }
  }
}

/* Свіжий наростаючий фронт PCLK. Лічильник ітерацій замість DWT — так
 * внутрішній цикл коротший і встигає за PCLK. 0 — PCLK не тактує. */
static inline int wait_pclk_rising(uint32_t spin)
{
  while (dvp_pclk() != 0U)
  {
    if (spin-- == 0U)
    {
      return 0;
    }
  }
  while (dvp_pclk() == 0U)
  {
    if (spin-- == 0U)
    {
      return 0;
    }
  }
  return 1;
}

platform_cam_status_t Platform_CameraCapture(const platform_cam_cfg_t *cfg, uint8_t *buf,
                                             size_t cap, platform_cam_result_t *res)
{
  platform_cam_result_t r = { 0U, 0U, 0U, 0U, 0U };
  platform_cam_status_t st = PLATFORM_CAM_OK;
  uint32_t line_bytes;
  uint32_t vsync_cyc;
  uint32_t href_cyc;
  uint32_t t0;

  if (res != NULL)
  {
    *res = r;
  }
  if ((cfg == NULL) || (buf == NULL) || (cfg->width == 0U) || (cfg->height == 0U) ||
      (cfg->bytes_per_pixel == 0U) || (cfg->pclk_spin_max == 0U))
  {
    return PLATFORM_CAM_ERR_ARG;
  }
  line_bytes = (uint32_t)cfg->width * cfg->bytes_per_pixel;
  if (cap < (size_t)line_bytes * cfg->height)
  {
    return PLATFORM_CAM_ERR_OVERFLOW;
  }

  vsync_cyc = us_to_cycles(cfg->vsync_timeout_us);
  href_cyc  = us_to_cycles(cfg->href_timeout_us);
  t0 = Platform_Cycles();

  /* Початок кадру: якщо зайшли посеред імпульсу — дочекатись його кінця,
   * потім наступного повноцінного імпульсу і його спаду. Тут чекаємо до одного кадру. */
  if (!wait_frame_start(us_to_cycles(cfg->vsync_min_us), vsync_cyc, &r.vsync_glitches))
  {
    st = PLATFORM_CAM_ERR_VSYNC_TIMEOUT;
  }

  for (uint32_t y = 0U; (y < cfg->height) && (st == PLATFORM_CAM_OK); y++)
  {
    uint8_t *p = buf + (size_t)y * line_bytes;
    uint32_t primask = 0U;
    uint32_t i;

    /* Гасіння між лініями — з увімкненими перериваннями: SysTick не губиться */
    if (!wait_href_high(href_cyc))
    {
      st = PLATFORM_CAM_ERR_HREF_TIMEOUT;
      break;
    }
    if (cfg->irq_lock_per_line != 0U)
    {
      primask = dvp_irq_save();
    }

    for (i = 0U; i < line_bytes; i++)
    {
      uint32_t w;

      if (!wait_pclk_rising(cfg->pclk_spin_max))
      {
        st = PLATFORM_CAM_ERR_PCLK_TIMEOUT;
        break;
      }
      w = dvp_read();
      if (DVP_HREF(w) == 0U)
      {
        break;   /* лінія закінчилась раніше: кадр іншого розміру, ніж налаштовано */
      }
      p[i] = DVP_DATA(w);
    }

    if (st == PLATFORM_CAM_OK)
    {
      /* Решту лінії (якщо вона довша) пропускаємо: чекаємо спаду HREF */
      uint32_t spin = cfg->pclk_spin_max;
      while ((DVP_HREF(dvp_read()) != 0U) && (spin-- != 0U))
      {
      }
    }
    if (cfg->irq_lock_per_line != 0U)
    {
      dvp_irq_restore(primask);
    }

    r.bytes += i;
    r.lines++;
    if (i < line_bytes)
    {
      r.short_lines++;
    }
  }

  if ((st == PLATFORM_CAM_OK) && (r.short_lines != 0U))
  {
    st = PLATFORM_CAM_ERR_SHORT_FRAME;
  }
  r.cycles = Platform_Cycles() - t0;
  if (res != NULL)
  {
    *res = r;
  }
  return st;
}
