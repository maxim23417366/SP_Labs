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
  */
#ifndef FAKE_PLATFORM_H
#define FAKE_PLATFORM_H

#include <stddef.h>
#include <stdint.h>

#include "dev_bus.h"

typedef struct
{
  uint32_t     now_ms;            /* Platform_Millis(); росте від Platform_DelayMs() */
  uint32_t     delay_total_ms;    /* сума всіх затримок */

  uint32_t     inputs;            /* Platform_ReadInputs(): біти PLATFORM_IN_* */

  dev_status_t dht_status;        /* != DEV_OK — Platform_Dht11Capture() повертає цю помилку */
  uint16_t     dht_high_us[40];

  uint32_t     servo_freq_hz;
  uint32_t     servo_pulse_us;

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

#endif /* FAKE_PLATFORM_H */
