/**
  ******************************************************************************
  * @file    periph_check.c
  * @brief   Перевірка ініціалізації периферії (ЛР1, крок 6).
  *
  *          Результат видно трьома способами:
  *            - LD2: повільно (1 Гц) — усе OK; швидко (5 Гц) — якийсь пристрій
  *              не відповідає; світиться постійно — спрацював Error_Handler();
  *            - змінна periph_check у дебагері;
  *            - лог через printf, який Platform/retarget.c відправляє в USART2.
  ******************************************************************************
  */
#include "periph_check.h"

#include <stdio.h>

#include "main.h"
#include "i2c.h"
#include "spi.h"
#include "tim.h"
#include "usart.h"

/* Очікувані частоти — з Clock Configuration у firmware.ioc */
#define EXPECTED_SYSCLK_HZ      96000000UL
#define EXPECTED_HCLK_HZ        96000000UL
#define EXPECTED_PCLK1_HZ       48000000UL
#define EXPECTED_PCLK2_HZ       96000000UL

/* I2C1: діапазон сканування без зарезервованих адрес */
#define I2C_ADDR_FIRST          0x08U
#define I2C_ADDR_LAST           0x77U
#define I2C_ADDR_OV7670         0x21U
#define I2C_ADDR_MPU6050        0x68U
#define I2C_PROBE_TRIALS        2U
#define I2C_PROBE_TIMEOUT_MS    5U

/* SPI2: W25Q64, команда Read JEDEC ID */
#define W25_CMD_JEDEC_ID        0x9FU
#define W25_MANUFACTURER_ID     0xEFU   /* Winbond */
#define SPI_TIMEOUT_MS          10U

/* TIM3 CH1: сервопривід SG92R */
#define PWM_EXPECTED_FREQ_HZ    50U
#define PWM_EXPECTED_PULSE_US   1500U

#define LOOP_PERIOD_MS          500U
#define LED_FAST_PERIOD_MS      100U

PeriphCheck_Result periph_check;

static const char *ok_str(uint8_t ok)
{
  return ok ? "OK" : "FAIL";
}

static const char *ready_str(uint8_t ready)
{
  return ready ? "READY" : "NOT READY";
}

static const char *i2c_device_name(uint8_t addr)
{
  switch (addr)
  {
    case 0x1DU: case 0x1EU: return "LSM303D accel/mag (GY-89)";
    case 0x21U:             return "OV7670 camera (SCCB)";
    case 0x68U:             return "MPU-6050";
    case 0x69U:             return "MPU-6050 (AD0 = 1)";
    case 0x6AU: case 0x6BU: return "L3GD20 gyro (GY-89)";
    case 0x77U:             return "BMP180 baro (GY-89)";
    default:                return "unknown device";
  }
}

static void update_summary(void)
{
  periph_check.all_ok = periph_check.init_ok && periph_check.clocks_ok &&
                        periph_check.i2c_ok && periph_check.flash_ok &&
                        periph_check.pwm_ok;
}

/* Якщо будь-яка MX_*_Init() не повернула HAL_OK, згенерований код уже викликав
 * Error_Handler() і сюди ми б не дійшли. Стан READY кожного хендла — додаткове
 * підтвердження, що модулі справді ініціалізовані. */
static void check_init(void)
{
  uint8_t i2c  = (hi2c1.State   == HAL_I2C_STATE_READY);
  uint8_t spi  = (hspi2.State   == HAL_SPI_STATE_READY);
  uint8_t tim  = (htim3.State   == HAL_TIM_STATE_READY);
  uint8_t uart = (huart2.gState == HAL_UART_STATE_READY);

  periph_check.init_ok = i2c && spi && tim && uart;

  printf("[INIT ] I2C1 %s, SPI2 %s, TIM3 %s, USART2 %s -> %s\r\n",
         ready_str(i2c), ready_str(spi), ready_str(tim), ready_str(uart),
         ok_str(periph_check.init_ok));
}

static void check_clocks(void)
{
  periph_check.sysclk_hz = HAL_RCC_GetSysClockFreq();
  periph_check.hclk_hz   = HAL_RCC_GetHCLKFreq();
  periph_check.pclk1_hz  = HAL_RCC_GetPCLK1Freq();
  periph_check.pclk2_hz  = HAL_RCC_GetPCLK2Freq();

  periph_check.clocks_ok = (periph_check.sysclk_hz == EXPECTED_SYSCLK_HZ) &&
                           (periph_check.hclk_hz   == EXPECTED_HCLK_HZ)   &&
                           (periph_check.pclk1_hz  == EXPECTED_PCLK1_HZ)  &&
                           (periph_check.pclk2_hz  == EXPECTED_PCLK2_HZ);

  printf("[CLOCK] SYSCLK=%lu HCLK=%lu PCLK1=%lu PCLK2=%lu Hz -> %s\r\n",
         (unsigned long)periph_check.sysclk_hz, (unsigned long)periph_check.hclk_hz,
         (unsigned long)periph_check.pclk1_hz, (unsigned long)periph_check.pclk2_hz,
         ok_str(periph_check.clocks_ok));
}

static void check_i2c(void)
{
  periph_check.i2c_found_count = 0U;
  periph_check.i2c_bus_busy    = 0U;
  periph_check.mpu6050_present = 0U;
  periph_check.ov7670_present  = 0U;

  printf("[I2C1 ] scan 0x%02X..0x%02X:\r\n", I2C_ADDR_FIRST, I2C_ADDR_LAST);

  for (uint8_t addr = I2C_ADDR_FIRST; addr <= I2C_ADDR_LAST; addr++)
  {
    HAL_StatusTypeDef st = HAL_I2C_IsDeviceReady(&hi2c1, (uint16_t)(addr << 1),
                                                 I2C_PROBE_TRIALS, I2C_PROBE_TIMEOUT_MS);
    if (st == HAL_BUSY)
    {
      /* BUSY не знімається: немає підтяжок (модулі не підключені)
       * або SDA/SCL притиснуті до землі. Далі сканувати немає сенсу. */
      periph_check.i2c_bus_busy = 1U;
      break;
    }
    if (st != HAL_OK)
    {
      continue;
    }

    if (periph_check.i2c_found_count < PERIPH_CHECK_I2C_MAX_FOUND)
    {
      periph_check.i2c_found[periph_check.i2c_found_count] = addr;
    }
    periph_check.i2c_found_count++;

    if (addr == I2C_ADDR_MPU6050)
    {
      periph_check.mpu6050_present = 1U;
    }
    if (addr == I2C_ADDR_OV7670)
    {
      periph_check.ov7670_present = 1U;
    }
    printf("        0x%02X  %s\r\n", addr, i2c_device_name(addr));
  }

  if (periph_check.i2c_bus_busy)
  {
    printf("        bus BUSY: no pull-ups or SDA/SCL shorted to GND\r\n");
  }
  if (!periph_check.mpu6050_present)
  {
    printf("        MISSING 0x68 MPU-6050\r\n");
  }
  if (!periph_check.ov7670_present)
  {
    printf("        MISSING 0x21 OV7670 (check XCLK 24 MHz on PA8, PWDN low, RESET high)\r\n");
  }

  periph_check.i2c_ok = !periph_check.i2c_bus_busy &&
                        periph_check.mpu6050_present && periph_check.ov7670_present;

  printf("        %u device(s) found -> %s\r\n",
         periph_check.i2c_found_count, ok_str(periph_check.i2c_ok));
}

static uint8_t read_jedec_id(uint8_t id[3])
{
  uint8_t tx[4] = { W25_CMD_JEDEC_ID, 0xFFU, 0xFFU, 0xFFU };
  uint8_t rx[4] = { 0U };

  HAL_GPIO_WritePin(FLASH_CS_GPIO_Port, FLASH_CS_Pin, GPIO_PIN_RESET);
  HAL_StatusTypeDef st = HAL_SPI_TransmitReceive(&hspi2, tx, rx, sizeof(tx), SPI_TIMEOUT_MS);
  HAL_GPIO_WritePin(FLASH_CS_GPIO_Port, FLASH_CS_Pin, GPIO_PIN_SET);

  id[0] = rx[1];
  id[1] = rx[2];
  id[2] = rx[3];

  /* Без мікросхеми MISO «висить» і читається FF FF FF або 00 00 00 */
  return (st == HAL_OK) && (id[0] == W25_MANUFACTURER_ID);
}

static void check_flash(void)
{
  uint8_t *id = periph_check.jedec_id;

  periph_check.flash_ok = read_jedec_id(id);

  printf("[SPI2 ] W25Q64 JEDEC ID: %02X %02X %02X", id[0], id[1], id[2]);
  if (periph_check.flash_ok && (id[2] >= 17U) && (id[2] <= 26U))
  {
    /* Байт ємності = log2(розміру в байтах): 0x17 -> 8 МБ -> 64 Мбіт */
    printf(" (Winbond, %lu Mbit)", 1UL << (id[2] - 17U));
  }
  printf(" -> %s\r\n", ok_str(periph_check.flash_ok));
}

static void check_pwm(void)
{
  HAL_StatusTypeDef st = HAL_TIM_PWM_Start(&htim3, TIM_CHANNEL_1);

  /* TIM3 на APB1: якщо дільник APB1 != 1, таймер тактується подвоєною PCLK1 */
  uint32_t tim_clk = HAL_RCC_GetPCLK1Freq();
  if ((RCC->CFGR & RCC_CFGR_PPRE1) != RCC_CFGR_PPRE1_DIV1)
  {
    tim_clk *= 2U;
  }

  uint32_t tick_hz = tim_clk / (htim3.Instance->PSC + 1U);
  uint32_t period  = htim3.Instance->ARR + 1U;
  uint32_t pulse  = htim3.Instance->CCR1;

  periph_check.pwm_freq_hz  = tick_hz / period;
  periph_check.pwm_pulse_us = (uint32_t)(((uint64_t)pulse * 1000000U) / tick_hz);

  uint8_t running = ((htim3.Instance->CR1 & TIM_CR1_CEN) != 0U) &&
                    ((htim3.Instance->CCER & TIM_CCER_CC1E) != 0U);

  periph_check.pwm_ok = (st == HAL_OK) && running &&
                        (periph_check.pwm_freq_hz == PWM_EXPECTED_FREQ_HZ) &&
                        (periph_check.pwm_pulse_us == PWM_EXPECTED_PULSE_US);

  printf("[TIM3 ] PWM CH1 (PA6): %lu Hz, pulse %lu us -> %s\r\n",
         (unsigned long)periph_check.pwm_freq_hz, (unsigned long)periph_check.pwm_pulse_us,
         ok_str(periph_check.pwm_ok));
}

void PeriphCheck_RunAll(void)
{
  printf("\r\n=== SmartLock: peripheral check (LR1, step 6) ===\r\n");

  check_init();
  check_clocks();
  check_i2c();
  check_flash();
  check_pwm();
  update_summary();

  printf("=== RESULT: %s ===\r\n", periph_check.all_ok ? "ALL OK" : "FAIL, see above");
}

void PeriphCheck_Loop(void)
{
  static uint32_t last_probe_ms = 0U;
  static uint32_t last_led_ms   = 0U;
  uint32_t now = HAL_GetTick();

  if ((now - last_probe_ms) >= LOOP_PERIOD_MS)
  {
    last_probe_ms = now;
    periph_check.loop_count++;

    /* Постійна активність для логічного аналізатора:
     *   I2C1: START, 0x68+W, ACK/NACK, STOP (PB8 = SCL, PB9 = SDA)
     *   SPI2: CS вниз, 9F + 3 байти, CS вгору (PB10 = CS, PB13 = SCK)
     * Заодно видно, якщо пристрій від'єднали або підключили на ходу. */
    HAL_StatusTypeDef st = HAL_I2C_IsDeviceReady(&hi2c1, (uint16_t)(I2C_ADDR_MPU6050 << 1),
                                                 1U, I2C_PROBE_TIMEOUT_MS);
    uint8_t mpu   = (st == HAL_OK);
    uint8_t flash = read_jedec_id(periph_check.jedec_id);

    if ((mpu != periph_check.mpu6050_present) || (flash != periph_check.flash_ok))
    {
      periph_check.mpu6050_present = mpu;
      periph_check.flash_ok        = flash;
      periph_check.i2c_bus_busy    = (st == HAL_BUSY);
      periph_check.i2c_ok = !periph_check.i2c_bus_busy &&
                            periph_check.mpu6050_present && periph_check.ov7670_present;
      update_summary();

      printf("[LOOP ] MPU-6050 %s, W25Q64 %s -> %s\r\n",
             mpu ? "present" : "missing", flash ? "present" : "missing",
             periph_check.all_ok ? "ALL OK" : "FAIL");
    }
  }

  uint32_t led_period = periph_check.all_ok ? LOOP_PERIOD_MS : LED_FAST_PERIOD_MS;
  if ((now - last_led_ms) >= led_period)
  {
    last_led_ms = now;
    HAL_GPIO_TogglePin(LD2_GPIO_Port, LD2_Pin);
  }
}

void PeriphCheck_OnError(void)
{
  /* Error_Handler() може спрацювати ще до MX_GPIO_Init() — наприклад, якщо
   * в SystemClock_Config() не запустився HSE від ST-Link. Тому LD2 тут
   * налаштовується самостійно і не залежить від решти ініціалізації. */
  GPIO_InitTypeDef gpio = { 0 };

  __HAL_RCC_GPIOA_CLK_ENABLE();
  gpio.Pin   = LD2_Pin;
  gpio.Mode  = GPIO_MODE_OUTPUT_PP;
  gpio.Pull  = GPIO_NOPULL;
  gpio.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(LD2_GPIO_Port, &gpio);
  HAL_GPIO_WritePin(LD2_GPIO_Port, LD2_Pin, GPIO_PIN_SET);

  while (1)
  {
  }
}
