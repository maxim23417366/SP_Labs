/**
  * @file    fake_dvp.c
  * @brief   Генератор сигналів VSYNC / HREF / PCLK / D0..D7 (див. fake_dvp.h).
  */
#include "fake_dvp.h"

#include <string.h>

#include "fake_dvp_port.h"
#include "fake_platform.h"

fake_dvp_t fake_dvp;

static int pclk_dead;   /* PCLK завмер і більше не тактує (до fake_dvp_reset) */

void fake_dvp_reset(uint32_t line_bytes, uint32_t height)
{
  memset(&fake_dvp, 0, sizeof(fake_dvp));
  pclk_dead = 0;
  fake_dvp.line_bytes      = line_bytes;
  fake_dvp.height          = height;
  fake_dvp.half_period     = 2U;
  fake_dvp.vsync_steps     = 20U;
  fake_dvp.gap_steps       = 30U;
  fake_dvp.blank_steps     = 12U;
  fake_dvp.tail_steps      = 25U;
  fake_dvp.cycles_per_read = 10U;
  fake_dvp.short_line      = -1;
  fake_dvp.dead_pclk_line  = -1;
  fake_dvp.no_href_after   = -1;
}

uint8_t fake_dvp_byte(uint32_t y, uint32_t x)
{
  return (uint8_t)(y * 31U + x * 7U + 3U);
}

static uint32_t bytes_in_line(uint32_t y)
{
  uint32_t n = fake_dvp.line_bytes + fake_dvp.extra_bytes;

  if ((fake_dvp.short_line >= 0) && (y == (uint32_t)fake_dvp.short_line))
  {
    n -= 3U;
  }
  return n;
}

static uint32_t frame_steps(void)
{
  uint32_t n = fake_dvp.vsync_steps + fake_dvp.gap_steps + fake_dvp.tail_steps;

  for (uint32_t y = 0U; y < fake_dvp.height; y++)
  {
    n += fake_dvp.blank_steps + bytes_in_line(y) * 2U * fake_dvp.half_period;
  }
  return n;
}

/* Стан ліній у момент кроку s */
typedef struct
{
  uint32_t vsync;
  uint32_t href;
  uint32_t pclk;
  uint8_t  data;
  int32_t  line;   /* номер лінії в активній частині, -1 — гасіння */
} dvp_state_t;

static dvp_state_t state_at(uint32_t s)
{
  dvp_state_t st = { 0U, 0U, 0U, 0U, -1 };
  uint32_t hp = fake_dvp.half_period;
  uint32_t t = s % frame_steps();

  st.pclk = (t / hp) & 1U;   /* вільний PCLK у гасінні */
  if (t < fake_dvp.vsync_steps)
  {
    st.vsync = fake_dvp.no_vsync ? 0U : 1U;
    return st;
  }
  t -= fake_dvp.vsync_steps;
  if (t < fake_dvp.gap_steps)
  {
    return st;
  }
  t -= fake_dvp.gap_steps;

  for (uint32_t y = 0U; y < fake_dvp.height; y++)
  {
    uint32_t active = bytes_in_line(y) * 2U * hp;

    if (t < fake_dvp.blank_steps)
    {
      return st;
    }
    t -= fake_dvp.blank_steps;
    if (t < active)
    {
      uint32_t x = t / (2U * hp);

      if ((fake_dvp.no_href_after >= 0) && (y >= (uint32_t)fake_dvp.no_href_after))
      {
        return st;
      }
      st.href = 1U;
      st.line = (int32_t)y;
      st.data = fake_dvp_byte(y, x);
      /* байт x: спершу hp кроків PCLK = 0, потім hp кроків PCLK = 1 (фронт = дані валідні) */
      st.pclk = ((t % (2U * hp)) >= hp) ? 1U : 0U;
      return st;
    }
    t -= active;
  }
  return st;   /* хвіст кадру */
}

static dvp_state_t step(void)
{
  uint32_t s = fake_dvp.start_step + fake_dvp.step;
  dvp_state_t st = state_at(s);
  uint32_t t = s % frame_steps();

  /* Завада на VSYNC (лог 15 ЛР3): короткий імпульс посеред кадру */
  if ((fake_dvp.glitch_steps != 0U) && (t >= fake_dvp.glitch_at) &&
      (t < (fake_dvp.glitch_at + fake_dvp.glitch_steps)))
  {
    st.vsync = 1U;
  }

  /* Несправність «PCLK зупинився»: з початку лінії dead_pclk_line і назавжди */
  if ((fake_dvp.dead_pclk_line >= 0) && (st.line >= fake_dvp.dead_pclk_line))
  {
    pclk_dead = 1;
  }
  if (pclk_dead)
  {
    st.pclk = 0U;
  }

  fake_dvp.step++;
  fake_platform.cycles += fake_dvp.cycles_per_read;
  return st;
}

uint32_t fake_dvp_read(void)
{
  dvp_state_t st = step();

  return (uint32_t)st.data | (st.vsync << 8) | (st.href << 10);
}

uint32_t fake_dvp_pclk(void)
{
  return step().pclk;
}

uint32_t fake_dvp_irq_save(void)
{
  fake_dvp.irq_saves++;
  fake_dvp.irq_depth++;
  if (fake_dvp.irq_depth > fake_dvp.irq_depth_max)
  {
    fake_dvp.irq_depth_max = fake_dvp.irq_depth;
  }
  return 0U;
}

void fake_dvp_irq_restore(uint32_t primask)
{
  (void)primask;
  fake_dvp.irq_restores++;
  fake_dvp.irq_depth--;
}
