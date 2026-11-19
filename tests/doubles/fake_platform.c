/**
  * @file    fake_platform.c
  * @brief   Реалізація platform.h для ПК (див. fake_platform.h).
  */
#include "fake_platform.h"

#include <string.h>

#include "fake_i2c.h"
#include "mock_spi.h"
#include "platform.h"

fake_platform_t fake_platform;

void fake_platform_reset(void)
{
  memset(&fake_platform, 0, sizeof(fake_platform));
  fake_platform.stack_reserved = 1024U;
  fake_platform.inputs         = PLATFORM_IN_BTN1 | PLATFORM_IN_BTN2 | PLATFORM_IN_BTN3 |
                                 PLATFORM_IN_BTN4 | PLATFORM_IN_LOCK | PLATFORM_IN_DOOR;
  fake_platform.dht_status     = DEV_ERR_TIMEOUT;
  fake_platform.servo_freq_hz  = 50U;
  fake_platform.servo_pulse_us = 1500U;
  fake_platform.sccb_status    = DEV_OK;
}

const dev_i2c_bus_t *Platform_SensorI2c(void)
{
  return fake_i2c_bus();
}

const dev_spi_bus_t *Platform_FlashSpi(void)
{
  return mock_spi_bus();
}

void Platform_DelayMs(uint32_t ms)
{
  fake_platform.now_ms         += ms;
  fake_platform.delay_total_ms += ms;
}

uint32_t Platform_Millis(void)
{
  return fake_platform.now_ms;
}

uint32_t Platform_Cycles(void)
{
  fake_platform.cycles_calls++;
  return (fake_platform.now_ms * 1000U) + (fake_platform.cycles_calls * fake_platform.cycles_step) +
         fake_platform.cycles;
}

uint32_t Platform_CyclesToUs(uint32_t cycles)
{
  return cycles;
}

void Platform_ProbeWrite(unsigned ch, unsigned level)
{
  if (ch >= PLATFORM_PROBE_COUNT)
  {
    return;
  }
  if ((level != 0U) && (fake_platform.probe_level[ch] == 0U))
  {
    fake_platform.probe_rises[ch]++;
  }
  fake_platform.probe_level[ch] = (uint8_t)(level != 0U);
}

/* 1 такт = 1 мкс, як і в Platform_Cycles() вище; тест камери (I07) задає 96 МГц */
uint32_t Platform_CpuHz(void)
{
  return (fake_platform.cpu_hz != 0U) ? fake_platform.cpu_hz : 1000000U;
}

void Platform_BringUp(void)
{
  fake_platform.bringup_calls++;
}

void Platform_Poll(void)
{
  fake_platform.poll_calls++;
}

void Platform_IoInit(void)
{
}

uint32_t Platform_ReadInputs(void)
{
  return fake_platform.inputs;
}

/* ---- ЛР3: помічники ------------------------------------------------------------ */
void fake_platform_advance_ms(uint32_t ms)
{
  fake_platform.now_ms += ms;
}

void fake_platform_set_inputs(uint32_t mask, int high)
{
  if (high != 0)
  {
    fake_platform.inputs |= mask;
  }
  else
  {
    fake_platform.inputs &= ~mask;
  }
}

static void dht_encode(const uint8_t frame[5], uint16_t high_us[40])
{
  for (unsigned i = 0U; i < 40U; i++)
  {
    high_us[i] = (frame[i / 8U] & (0x80U >> (i % 8U))) ? 72U : 24U;
  }
}

void fake_platform_dht_frame(const uint8_t frame[5])
{
  dht_encode(frame, fake_platform.dht_high_us);
  fake_platform.dht_status = DEV_OK;
}

void fake_platform_dht_values(uint8_t rh_pct, uint8_t temp_c)
{
  uint8_t f[5] = { rh_pct, 0U, temp_c, 0U, 0U };

  f[4] = (uint8_t)(f[0] + f[1] + f[2] + f[3]);
  fake_platform_dht_frame(f);
}

void fake_platform_dht_script(const fake_dht_step_t *steps, size_t n)
{
  fake_platform.dht_script     = (n > 0U) ? steps : NULL;
  fake_platform.dht_script_len = n;
  fake_platform.dht_script_pos = 0U;
}

dev_status_t Platform_Dht11Capture(uint16_t high_us[40])
{
  fake_platform.dht_calls++;
  if (fake_platform.dht_script != NULL)
  {
    size_t i = fake_platform.dht_script_pos;
    const fake_dht_step_t *s;

    if (i >= fake_platform.dht_script_len)
    {
      i = fake_platform.dht_script_len - 1U;
    }
    else
    {
      fake_platform.dht_script_pos++;
    }
    s = &fake_platform.dht_script[i];
    if (s->status != DEV_OK)
    {
      return s->status;
    }
    dht_encode(s->frame, high_us);
    return DEV_OK;
  }
  if (fake_platform.dht_status != DEV_OK)
  {
    return fake_platform.dht_status;
  }
  memcpy(high_us, fake_platform.dht_high_us, sizeof(fake_platform.dht_high_us));
  return DEV_OK;
}

void Platform_ServoSetPulseUs(uint32_t us)
{
  if (fake_platform.servo_set_calls < FAKE_SERVO_LOG_LEN)
  {
    fake_platform.servo_log[fake_platform.servo_set_calls]    = us;
    fake_platform.servo_log_ms[fake_platform.servo_set_calls] = fake_platform.now_ms;
  }
  fake_platform.servo_set_calls++;
  fake_platform.servo_pulse_us = us;
}

uint32_t Platform_ServoPulseUs(void)
{
  return fake_platform.servo_pulse_us;
}

uint32_t Platform_ServoFreqHz(void)
{
  return fake_platform.servo_freq_hz;
}

dev_status_t Platform_SccbRead(uint8_t addr7, uint8_t reg, uint8_t *val)
{
  uint8_t *regs = fake_i2c_device_regs(addr7);

  if (fake_platform.sccb_status != DEV_OK)
  {
    return fake_platform.sccb_status;
  }
  if (regs == NULL)
  {
    return DEV_ERR_NACK;
  }
  *val = regs[reg];
  return DEV_OK;
}

dev_status_t Platform_SccbWrite(uint8_t addr7, uint8_t reg, uint8_t val)
{
  uint8_t *regs = fake_i2c_device_regs(addr7);

  if (fake_platform.sccb_status != DEV_OK)
  {
    return fake_platform.sccb_status;
  }
  if (regs == NULL)
  {
    return DEV_ERR_NACK;
  }
  regs[reg] = val;
  fake_platform.sccb_last_reg = reg;
  fake_platform.sccb_last_val = val;
  return DEV_OK;
}

uint32_t Platform_VsyncCount(void)
{
  return (uint32_t)(((uint64_t)fake_platform.now_ms * fake_platform.vsync_hz) / 1000U);
}

dev_status_t Platform_I2cRecover(void)
{
  fake_platform.i2c_recover_calls++;
  return fake_platform.i2c_recover_status;
}

void Platform_I2cStats(platform_i2c_stats_t *out)
{
  *out = fake_platform.i2c_stats;
}

dev_status_t Platform_I2cProbe(uint8_t addr7)
{
  return (fake_i2c_device_regs(addr7) != NULL) ? DEV_OK : DEV_ERR_NACK;
}

int Platform_UartGetChar(void)
{
  unsigned char c;

  if ((fake_platform.uart_rx == NULL) || (fake_platform.uart_rx[fake_platform.uart_pos] == '\0'))
  {
    return PLATFORM_UART_NONE;
  }
  c = (unsigned char)fake_platform.uart_rx[fake_platform.uart_pos++];
  return (c == (unsigned char)FAKE_UART_LOST) ? PLATFORM_UART_LOST : (int)c;
}

void Platform_SetLoopTraffic(int on)
{
  fake_platform.loop_traffic = on;
}

/* ---- ЛР3 (I06): пам'ять ---------------------------------------------------------- */
void Platform_StackPaint(void)
{
  fake_platform.stack_paint_calls++;
}

uint32_t Platform_StackUsedBytes(void)
{
  return fake_platform.stack_used;
}

uint32_t Platform_StackReservedBytes(void)
{
  return fake_platform.stack_reserved;
}

void Platform_HeapInfo(platform_heap_t *heap)
{
  if (heap != NULL)
  {
    *heap = fake_platform.heap;
  }
}

/* ---- ЛР3 (I04): події з ISR і втрати UART ---------------------------------------- */
void fake_platform_raise_event(platform_event_t ev, uint32_t t_cycles)
{
  if (fake_platform.ev_pending[ev] != 0U)
  {
    fake_platform.ev_overruns[ev]++;
    return;
  }
  fake_platform.ev_t[ev] = t_cycles;
  fake_platform.ev_pending[ev] = 1U;
}

int Platform_TakeEvent(platform_event_t ev, uint32_t *t_cycles)
{
  if ((ev >= PLATFORM_EV_COUNT) || (fake_platform.ev_pending[ev] == 0U))
  {
    return 0;
  }
  if (t_cycles != NULL)
  {
    *t_cycles = fake_platform.ev_t[ev];
  }
  fake_platform.ev_pending[ev] = 0U;
  return 1;
}

uint32_t Platform_EventOverruns(platform_event_t ev)
{
  return (ev < PLATFORM_EV_COUNT) ? fake_platform.ev_overruns[ev] : 0U;
}

uint32_t Platform_UartOverruns(void)
{
  return fake_platform.uart_overruns;
}

uint32_t Platform_UartRxPeak(void)
{
  return fake_platform.uart_rx_peak;
}
