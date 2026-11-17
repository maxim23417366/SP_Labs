/**
  * @file    test_camera_dvp.c
  * @brief   Алгоритм захоплення кадру Platform/camera_dvp.c на ПК.
  *
  *          Справжній camera_dvp.c компілюється з -DPLATFORM_DVP_FAKE: замість
  *          GPIOC/GPIOA він читає модель сигналів fake_dvp.c (VSYNC, HREF, PCLK,
  *          D0..D7), а час — fake DWT. Так перевіряються синхронізація кадру,
  *          побайтна вибірка за фронтом PCLK, тайм-аути і маскування переривань
  *          на лінію — до того, як Колодницький перевірить сигнали на стенді (H14).
  */
#include <string.h>

#include "unity.h"
#include "fake_dvp.h"
#include "fake_platform.h"
#include "platform.h"

#define W     8U
#define H     5U
#define BPP   2U
#define LINE  (W * BPP)

static uint8_t buf[LINE * H + 16U];
static platform_cam_cfg_t cfg;

void setUp(void)
{
  fake_platform_reset();
  fake_platform.cpu_hz = 96000000U;   /* SYSCLK стенда: us_to_cycles() як на платі */
  fake_dvp_reset(LINE, H);
  memset(buf, 0xA5, sizeof(buf));
  cfg.width = W;
  cfg.height = H;
  cfg.bytes_per_pixel = BPP;
  cfg.irq_lock_per_line = 1U;
  cfg.vsync_timeout_us = 1000U;   /* 96 000 тактів = 9 600 кроків моделі */
  cfg.vsync_min_us = 1U;          /* 96 тактів < 200 тактів справжнього імпульсу моделі */
  cfg.href_timeout_us = 200U;
  cfg.pclk_spin_max = 200U;
}

void tearDown(void)
{
}

static void assert_frame_ok(void)
{
  for (uint32_t y = 0U; y < H; y++)
  {
    for (uint32_t x = 0U; x < LINE; x++)
    {
      TEST_ASSERT_EQUAL_HEX8_MESSAGE(fake_dvp_byte(y, x), buf[y * LINE + x], "pixel byte");
    }
  }
  TEST_ASSERT_EQUAL_HEX8(0xA5U, buf[LINE * H]);   /* за межі кадру не писали */
}

static void test_full_frame_bytes_match(void)
{
  platform_cam_result_t r;

  TEST_ASSERT_EQUAL(PLATFORM_CAM_OK, Platform_CameraCapture(&cfg, buf, sizeof(buf), &r));
  assert_frame_ok();
  TEST_ASSERT_EQUAL_UINT32(LINE * H, r.bytes);
  TEST_ASSERT_EQUAL_UINT16(H, r.lines);
  TEST_ASSERT_EQUAL_UINT16(0U, r.short_lines);
  TEST_ASSERT_TRUE(r.cycles > 0U);
  /* переривання маскуються рівно на кожну лінію і завжди відновлюються */
  TEST_ASSERT_EQUAL_UINT32(H, fake_dvp.irq_saves);
  TEST_ASSERT_EQUAL_UINT32(H, fake_dvp.irq_restores);
  TEST_ASSERT_EQUAL_UINT32(1U, fake_dvp.irq_depth_max);
}

static void test_start_mid_frame_waits_for_next_vsync(void)
{
  platform_cam_result_t r;

  /* старт посеред імпульсу VSYNC і посеред ліній — кадр має бути цілим */
  fake_dvp.start_step = 7U;
  TEST_ASSERT_EQUAL(PLATFORM_CAM_OK, Platform_CameraCapture(&cfg, buf, sizeof(buf), &r));
  assert_frame_ok();
  setUp();
  fake_dvp.start_step = 200U;
  TEST_ASSERT_EQUAL(PLATFORM_CAM_OK, Platform_CameraCapture(&cfg, buf, sizeof(buf), &r));
  assert_frame_ok();
}

static void test_longer_lines_are_cut_and_slow_pclk(void)
{
  platform_cam_result_t r;

  fake_dvp.extra_bytes = 6U;      /* камера дає ширший кадр, ніж налаштовано */
  fake_dvp.half_period = 5U;      /* повільніший PCLK */
  cfg.irq_lock_per_line = 0U;
  TEST_ASSERT_EQUAL(PLATFORM_CAM_OK, Platform_CameraCapture(&cfg, buf, sizeof(buf), &r));
  assert_frame_ok();
  TEST_ASSERT_EQUAL_UINT32(0U, fake_dvp.irq_saves);
}

static void test_short_line_reported(void)
{
  platform_cam_result_t r;

  fake_dvp.short_line = 2;
  TEST_ASSERT_EQUAL(PLATFORM_CAM_ERR_SHORT_FRAME, Platform_CameraCapture(&cfg, buf, sizeof(buf), &r));
  TEST_ASSERT_EQUAL_UINT16(1U, r.short_lines);
  TEST_ASSERT_EQUAL_UINT16(H, r.lines);
  TEST_ASSERT_EQUAL_UINT32(LINE * H - 3U, r.bytes);
}

static void test_timeouts(void)
{
  platform_cam_result_t r;

  fake_dvp.no_vsync = 1;
  TEST_ASSERT_EQUAL(PLATFORM_CAM_ERR_VSYNC_TIMEOUT, Platform_CameraCapture(&cfg, buf, sizeof(buf), &r));
  TEST_ASSERT_EQUAL_UINT16(0U, r.lines);
  /* тайм-аут VSYNC = 1000 мкс = 96 000 тактів; один крок моделі — 10 тактів */
  TEST_ASSERT_UINT32_WITHIN(200U, 96000U, r.cycles);

  setUp();
  /* PCLK зупиняється посеред кадру, який захоплюємо: старт у хвості попереднього
   * кадру (20 + 30 + 5·(12 + 16·4) + 25 = 455 кроків), інакше алгоритм чесно
   * пропускає поточний кадр і помирає на лінії 0 наступного */
  fake_dvp.start_step = 440U;
  fake_dvp.dead_pclk_line = 3;
  TEST_ASSERT_EQUAL(PLATFORM_CAM_ERR_PCLK_TIMEOUT, Platform_CameraCapture(&cfg, buf, sizeof(buf), &r));
  TEST_ASSERT_EQUAL_UINT16(4U, r.lines);
  TEST_ASSERT_EQUAL_UINT32(fake_dvp.irq_saves, fake_dvp.irq_restores);

  setUp();
  fake_dvp.no_href_after = 1;
  cfg.href_timeout_us = 5U;   /* 480 тактів = 48 кроків: менше, ніж до наступного кадру */
  TEST_ASSERT_EQUAL(PLATFORM_CAM_ERR_HREF_TIMEOUT, Platform_CameraCapture(&cfg, buf, sizeof(buf), &r));
  TEST_ASSERT_EQUAL_UINT16(1U, r.lines);
}

static void test_vsync_glitch_rejected(void)
{
  platform_cam_result_t r;

  /* Старт у гасінні лінії 0; у гасінні лінії 2 (крок кадру 50 + 2·76 + 1) —
   * завада на VSYNC шириною 3 кроки = 30 тактів ≈ 0,3 мкс. Справжній імпульс —
   * 20 кроків = 200 тактів ≈ 2,1 мкс. */
  fake_dvp.start_step   = 55U;
  fake_dvp.glitch_at    = 203U;
  fake_dvp.glitch_steps = 3U;

  /* без фільтра завада приймається за початок кадру: кадр зшитий з двох */
  cfg.vsync_min_us = 0U;
  TEST_ASSERT_EQUAL(PLATFORM_CAM_OK, Platform_CameraCapture(&cfg, buf, sizeof(buf), &r));
  TEST_ASSERT_NOT_EQUAL(fake_dvp_byte(0U, 0U), buf[0]);
  TEST_ASSERT_EQUAL_UINT16(0U, r.vsync_glitches);

  /* з фільтром 1 мкс = 96 тактів: завада відкинута, кадр цілий */
  setUp();
  fake_dvp.start_step   = 55U;
  fake_dvp.glitch_at    = 203U;
  fake_dvp.glitch_steps = 3U;
  cfg.vsync_min_us = 1U;
  TEST_ASSERT_EQUAL(PLATFORM_CAM_OK, Platform_CameraCapture(&cfg, buf, sizeof(buf), &r));
  assert_frame_ok();
  TEST_ASSERT_EQUAL_UINT16(1U, r.vsync_glitches);

  /* лише завади, справжнього VSYNC немає: вихід за загальною межею 3 × тайм-аут */
  setUp();
  fake_dvp.no_vsync     = 1;
  fake_dvp.glitch_at    = 203U;
  fake_dvp.glitch_steps = 3U;
  cfg.vsync_min_us = 1U;
  TEST_ASSERT_EQUAL(PLATFORM_CAM_ERR_VSYNC_TIMEOUT, Platform_CameraCapture(&cfg, buf, sizeof(buf), &r));
  TEST_ASSERT_TRUE(r.vsync_glitches > 50U);           /* 288 000 тактів / 4 550 на кадр моделі */
  TEST_ASSERT_UINT32_WITHIN(10000U, 3U * 96000U, r.cycles);
  TEST_ASSERT_EQUAL_UINT16(0U, r.lines);
}

static void test_bad_arguments(void)
{
  platform_cam_result_t r;

  TEST_ASSERT_EQUAL(PLATFORM_CAM_ERR_OVERFLOW, Platform_CameraCapture(&cfg, buf, LINE * H - 1U, &r));
  TEST_ASSERT_EQUAL(PLATFORM_CAM_ERR_ARG, Platform_CameraCapture(NULL, buf, sizeof(buf), &r));
  TEST_ASSERT_EQUAL(PLATFORM_CAM_ERR_ARG, Platform_CameraCapture(&cfg, NULL, sizeof(buf), &r));
  cfg.height = 0U;
  TEST_ASSERT_EQUAL(PLATFORM_CAM_ERR_ARG, Platform_CameraCapture(&cfg, buf, sizeof(buf), &r));
  cfg.height = H;
  cfg.pclk_spin_max = 0U;
  TEST_ASSERT_EQUAL(PLATFORM_CAM_ERR_ARG, Platform_CameraCapture(&cfg, buf, sizeof(buf), NULL));
  cfg.pclk_spin_max = 200U;
  TEST_ASSERT_EQUAL(PLATFORM_CAM_OK, Platform_CameraCapture(&cfg, buf, sizeof(buf), NULL));
  TEST_ASSERT_EQUAL_UINT32(0U, fake_dvp.irq_saves - fake_dvp.irq_restores);
}

int main(void)
{
  UNITY_BEGIN();
  RUN_TEST(test_full_frame_bytes_match);
  RUN_TEST(test_start_mid_frame_waits_for_next_vsync);
  RUN_TEST(test_longer_lines_are_cut_and_slow_pclk);
  RUN_TEST(test_short_line_reported);
  RUN_TEST(test_timeouts);
  RUN_TEST(test_vsync_glitch_rejected);
  RUN_TEST(test_bad_arguments);
  return UNITY_END();
}
