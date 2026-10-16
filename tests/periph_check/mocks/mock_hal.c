/**
  * @file    mock_hal.c
  * @brief   Поведінка імітованого «заліза» для тестів periph_check.c.
  */
#include <string.h>

#include "main.h"

MockHal      mock;
GPIO_TypeDef mock_gpioa = { 0xAU };
GPIO_TypeDef mock_gpiob = { 0xBU };
RCC_TypeDef  mock_rcc;
TIM_TypeDef  mock_tim3;

I2C_HandleTypeDef  hi2c1;
SPI_HandleTypeDef  hspi2;
TIM_HandleTypeDef  htim3;
UART_HandleTypeDef huart2;

void mock_reset(void)
{
  uint32_t tick = mock.tick_ms;

  memset(&mock, 0, sizeof(mock));
  mock.tick_ms = tick;

  /* Пристрої на I2C1: камера, MPU-6050 і три чипи GY-89 */
  mock.i2c_ack[0x21] = 1U;   /* OV7670 */
  mock.i2c_ack[0x68] = 1U;   /* MPU-6050 */
  mock.i2c_ack[0x1D] = 1U;   /* LSM303D */
  mock.i2c_ack[0x6B] = 1U;   /* L3GD20 */
  mock.i2c_ack[0x77] = 1U;   /* BMP180 */
  mock.i2c_min_addr  = 0xFFU;

  /* W25Q64: Winbond, SPI NOR, 64 Мбіт */
  mock.jedec_id[0]    = 0xEFU;
  mock.jedec_id[1]    = 0x40U;
  mock.jedec_id[2]    = 0x17U;
  mock.spi_status     = HAL_OK;
  mock.flash_cs_level = 1U;  /* MX_GPIO_Init() ставить FLASH_CS у High */

  /* Тактування з firmware.ioc: 96 / 96 / 48 / 96 МГц, APB1 = /2 */
  mock.sysclk_hz = 96000000UL;
  mock.hclk_hz   = 96000000UL;
  mock.pclk1_hz  = 48000000UL;
  mock.pclk2_hz  = 96000000UL;
  mock_rcc.CFGR  = RCC_CFGR_PPRE1_DIV2;

  /* TIM3 після MX_TIM3_Init(): PSC 95, ARR 19999, CCR1 1500, лічильник зупинений */
  memset(&mock_tim3, 0, sizeof(mock_tim3));
  mock_tim3.PSC  = 95U;
  mock_tim3.ARR  = 19999U;
  mock_tim3.CCR1 = 1500U;
  mock.pwm_start_status = HAL_OK;

  hi2c1.State   = HAL_I2C_STATE_READY;
  hspi2.State   = HAL_SPI_STATE_READY;
  htim3.Instance = &mock_tim3;
  htim3.State   = HAL_TIM_STATE_READY;
  huart2.gState = HAL_UART_STATE_READY;
}

/* ---- GPIO ---------------------------------------------------------------- */
void HAL_GPIO_Init(GPIO_TypeDef *port, GPIO_InitTypeDef *init)
{
  (void)port;
  (void)init;
}

void HAL_GPIO_WritePin(GPIO_TypeDef *port, uint16_t pin, GPIO_PinState state)
{
  if ((port == FLASH_CS_GPIO_Port) && (pin == FLASH_CS_Pin))
  {
    mock.flash_cs_level = (state == GPIO_PIN_SET);
  }
  if ((port == LD2_GPIO_Port) && (pin == LD2_Pin))
  {
    mock.led_level = (state == GPIO_PIN_SET);
  }
}

void HAL_GPIO_TogglePin(GPIO_TypeDef *port, uint16_t pin)
{
  if ((port == LD2_GPIO_Port) && (pin == LD2_Pin))
  {
    mock.led_level = !mock.led_level;
    mock.led_toggles++;
  }
}

/* ---- RCC і час ----------------------------------------------------------- */
uint32_t HAL_RCC_GetSysClockFreq(void) { return mock.sysclk_hz; }
uint32_t HAL_RCC_GetHCLKFreq(void)     { return mock.hclk_hz; }
uint32_t HAL_RCC_GetPCLK1Freq(void)    { return mock.pclk1_hz; }
uint32_t HAL_RCC_GetPCLK2Freq(void)    { return mock.pclk2_hz; }
uint32_t HAL_GetTick(void)             { return mock.tick_ms; }

/* ---- I2C ----------------------------------------------------------------- */
HAL_StatusTypeDef HAL_I2C_IsDeviceReady(I2C_HandleTypeDef *hi2c, uint16_t dev_address,
                                        uint32_t trials, uint32_t timeout)
{
  (void)hi2c;
  (void)trials;
  (void)timeout;

  /* HAL очікує 8-бітну адресу (7-бітна, зсунута вліво) */
  uint8_t addr = (uint8_t)(dev_address >> 1);

  mock.i2c_probe_count++;
  if (addr < mock.i2c_min_addr)
  {
    mock.i2c_min_addr = addr;
  }
  if (addr > mock.i2c_max_addr)
  {
    mock.i2c_max_addr = addr;
  }

  if (mock.i2c_bus_busy)
  {
    return HAL_BUSY;
  }
  return mock.i2c_ack[addr & 0x7FU] ? HAL_OK : HAL_ERROR;
}

/* ---- SPI: поводиться як W25Q64 на команду Read JEDEC ID ------------------ */
HAL_StatusTypeDef HAL_SPI_TransmitReceive(SPI_HandleTypeDef *hspi, uint8_t *tx, uint8_t *rx,
                                          uint16_t size, uint32_t timeout)
{
  (void)hspi;
  (void)timeout;

  mock.flash_transfers++;

  /* Флешка слухає лише при опущеному CS і розуміє тут тільки 0x9F */
  if ((mock.flash_cs_level != 0U) || (size < 4U) || (tx[0] != 0x9FU))
  {
    mock.flash_protocol_errors++;
    memset(rx, 0xFF, size);
    return mock.spi_status;
  }

  rx[0] = 0xFFU;
  rx[1] = mock.jedec_id[0];
  rx[2] = mock.jedec_id[1];
  rx[3] = mock.jedec_id[2];
  return mock.spi_status;
}

/* ---- TIM ----------------------------------------------------------------- */
HAL_StatusTypeDef HAL_TIM_PWM_Start(TIM_HandleTypeDef *htim, uint32_t channel)
{
  (void)channel;

  if (mock.pwm_start_status == HAL_OK)
  {
    htim->Instance->CCER |= TIM_CCER_CC1E;
    htim->Instance->CR1  |= TIM_CR1_CEN;
  }
  return mock.pwm_start_status;
}

/* ---- UART ---------------------------------------------------------------- */
HAL_StatusTypeDef HAL_UART_Transmit(UART_HandleTypeDef *huart, const uint8_t *data,
                                    uint16_t size, uint32_t timeout)
{
  (void)timeout;

  mock.uart_last_handle = huart;
  for (uint16_t i = 0U; i < size; i++)
  {
    if (mock.uart_tx_count < sizeof(mock.uart_tx))
    {
      mock.uart_tx[mock.uart_tx_count] = data[i];
    }
    mock.uart_tx_count++;
  }
  return HAL_OK;
}
