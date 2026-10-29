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

dev_status_t Platform_Dht11Capture(uint16_t high_us[40])
{
  if (fake_platform.dht_status != DEV_OK)
  {
    return fake_platform.dht_status;
  }
  memcpy(high_us, fake_platform.dht_high_us, sizeof(fake_platform.dht_high_us));
  return DEV_OK;
}

void Platform_ServoSetPulseUs(uint32_t us)
{
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

dev_status_t Platform_I2cProbe(uint8_t addr7)
{
  return (fake_i2c_device_regs(addr7) != NULL) ? DEV_OK : DEV_ERR_NACK;
}

int Platform_UartGetChar(void)
{
  if ((fake_platform.uart_rx == NULL) || (fake_platform.uart_rx[fake_platform.uart_pos] == '\0'))
  {
    return -1;
  }
  return (unsigned char)fake_platform.uart_rx[fake_platform.uart_pos++];
}

void Platform_SetLoopTraffic(int on)
{
  fake_platform.loop_traffic = on;
}
