/**
  * @file    fake_platform.h
  * @brief   Fake шару Platform (firmware/Platform/platform.h) для хостових тестів App/.
  *
  *          App/ бачить залізо лише через platform.h, тому на ПК достатньо
  *          підмінити ці функції:
  *            - Platform_SensorI2c() -> fake_i2c (модель регістрів пристроїв);
  *            - Platform_FlashSpi()  -> mock_spi (заскриптований MISO);
  *            - Platform_I2cProbe() і Platform_Sccb*() відповідають за моделлю
  *              fake_i2c (пристрій є / немає), але в журнал транзакцій не пишуть;
  *            - час фіктивний: Platform_DelayMs() лише просуває Platform_Millis(),
  *              а лічильник VSYNC росте як vsync_hz × час;
  *            - решта (входи, DHT11, серво, UART RX) — поля fake_platform,
  *              які задає тест.
  *
  *          ЛР3 (K02–K06): журнал команд серво (servo_log), сценарій відповідей
  *          DHT11 (dht_script) і помічники для входів / часу / кадру DHT11.
  *          MPU-6050 і BMP180 через app_devices — fake_sensors.h.
  */
#ifndef FAKE_PLATFORM_H
#define FAKE_PLATFORM_H

#include <stddef.h>
#include <stdint.h>

#include "dev_bus.h"
#include "platform.h"

#define FAKE_SERVO_LOG_LEN  32U

/* Один крок сценарію DHT11: результат захоплення і 5 байт кадру (для DEV_OK) */
typedef struct
{
  dev_status_t status;
  uint8_t      frame[5];          /* RH, RH.dec, T, T.dec, CRC — як на дроті */
} fake_dht_step_t;

typedef struct
{
  uint32_t     now_ms;            /* Platform_Millis(); росте від Platform_DelayMs() */
  uint32_t     delay_total_ms;    /* сума всіх затримок */

  /* Platform_Cycles(): 1 такт = 1 мкс; значення = now_ms * 1000 + cycles_step на
   * кожен виклик. Так тест задає «тривалість» операції між двома зчитуваннями. */
  uint32_t     cycles_step;
  uint32_t     cycles_calls;

  uint8_t      probe_level[PLATFORM_PROBE_COUNT];    /* ЛР3 (I05): PROBE_ON/OFF — поточний рівень каналу */
  uint32_t     probe_rises[PLATFORM_PROBE_COUNT];    /* скільки разів канал піднімався в 1 */

  /* ЛР3 (I06): пам'ять */
  uint32_t     stack_used;
  uint32_t     stack_reserved;    /* після reset — 1024, як _Min_Stack_Size у лінкер-скрипті */
  platform_heap_t heap;
  unsigned     stack_paint_calls;

  uint32_t     inputs;            /* Platform_ReadInputs(): біти PLATFORM_IN_* */

  dev_status_t dht_status;        /* != DEV_OK — Platform_Dht11Capture() повертає цю помилку */
  uint16_t     dht_high_us[40];
  /* ЛР3: сценарій DHT11. Якщо dht_script != NULL, кожен Platform_Dht11Capture()
   * бере наступний крок; після останнього — знову останній (стале значення). */
  const fake_dht_step_t *dht_script;
  size_t       dht_script_len;
  size_t       dht_script_pos;
  uint32_t     dht_calls;         /* скільки разів викликано Platform_Dht11Capture() */

  uint32_t     servo_freq_hz;
  uint32_t     servo_pulse_us;
  /* ЛР3: кожен Platform_ServoSetPulseUs() — у журнал (перші FAKE_SERVO_LOG_LEN) */
  uint32_t     servo_set_calls;
  uint32_t     servo_log[FAKE_SERVO_LOG_LEN];
  uint32_t     servo_log_ms[FAKE_SERVO_LOG_LEN];   /* now_ms у момент команди */

  dev_status_t sccb_status;       /* != DEV_OK — примусова помилка SCCB */
  uint8_t      sccb_last_reg;     /* останній Platform_SccbWrite() */
  uint8_t      sccb_last_val;
  uint32_t     vsync_hz;          /* частота кадрів камери для Platform_VsyncCount() */

  const char  *uart_rx;           /* символи для Platform_UartGetChar(); NULL — порожньо */
  size_t       uart_pos;

  int          loop_traffic;      /* останній Platform_SetLoopTraffic() */
  unsigned     bringup_calls;
  unsigned     poll_calls;
} fake_platform_t;

extern fake_platform_t fake_platform;

/* Скидає поля до «справного стенда без пристроїв»: серво 50 Гц / 1500 мкс,
 * усі кнопки відпущені (1), DHT11 не відповідає (DEV_ERR_TIMEOUT), VSYNC 0 Гц.
 * fake_i2c і mock_spi тест скидає окремо. */
void fake_platform_reset(void);

/* ---- ЛР3: помічники для тестів K02–K06 --------------------------------------- */

/* Просуває Platform_Millis() без зміни delay_total_ms (час «між проходами»). */
void fake_platform_advance_ms(uint32_t ms);

/* Встановлює рівень бітів PLATFORM_IN_* (1 = HIGH). Кнопки й геркон активні
 * низьким рівнем: натиснуто / магніт поруч (двері зачинені) = 0; PIR: 1 = рух. */
void fake_platform_set_inputs(uint32_t mask, int high);

/* Кодує 5 байт кадру DHT11 у 40 тривалостей HIGH (1 = 72 мкс, 0 = 24 мкс;
 * поріг 48 мкс, як у stand_test.c) і ставить dht_status = DEV_OK. */
void fake_platform_dht_frame(const uint8_t frame[5]);

/* Кадр DHT11 з цілих значень: контрольна сума рахується сама. */
void fake_platform_dht_values(uint8_t rh_pct, uint8_t temp_c);

/* Сценарій відповідей DHT11 (див. dht_script); NULL — вимкнути. */
void fake_platform_dht_script(const fake_dht_step_t *steps, size_t n);

#endif /* FAKE_PLATFORM_H */
