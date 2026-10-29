/**
  ******************************************************************************
  * @file    dht11.c
  * @brief   DHT11: перетворення бітів у байти, контрольна сума, діапазони.
  ******************************************************************************
  */
#include "dht11.h"

#include <stddef.h>

void dht11_bits_to_bytes(const uint16_t high_us[40], uint16_t threshold_us,
                         uint8_t frame[DHT11_FRAME_LEN])
{
  for (uint32_t i = 0U; i < DHT11_FRAME_LEN; i++)
  {
    frame[i] = 0U;
  }
  /* Біти йдуть старшим першим: байт 0 = вологість, 1 = її десяті, 2 = температура,
   * 3 = її десяті, 4 = контрольна сума */
  for (uint32_t i = 0U; i < 40U; i++)
  {
    if (high_us[i] > threshold_us)
    {
      frame[i / 8U] |= (uint8_t)(0x80U >> (i % 8U));
    }
  }
}

dev_status_t dht11_decode(const uint8_t frame[DHT11_FRAME_LEN], dht11_data_t *out)
{
  if ((frame == NULL) || (out == NULL))
  {
    return DEV_ERR_PARAM;
  }

  uint8_t sum = (uint8_t)(frame[0] + frame[1] + frame[2] + frame[3]);
  if (sum != frame[4])
  {
    return DEV_ERR_STATE;
  }

  /* Паспорт DHT11: 20–90 % RH, 0–50 °C. Беремо з запасом, але відсікаємо
   * явне сміття на кшталт 255 % (лінія без підтяжки читається як усі одиниці). */
  if ((frame[0] > 100U) || (frame[2] > 60U))
  {
    return DEV_ERR_ID;
  }

  out->rh_pct     = frame[0];
  out->rh_tenth   = (uint8_t)(frame[1] % 10U);
  out->temp_c     = (int8_t)(frame[2] & 0x7FU);
  out->temp_tenth = (uint8_t)(frame[3] & 0x0FU);
  if ((frame[3] & 0x80U) != 0U)
  {
    out->temp_c = (int8_t)(-out->temp_c);
  }
  return DEV_OK;
}
