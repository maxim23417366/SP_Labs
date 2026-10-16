/**
  ******************************************************************************
  * @file    app.c
  * @brief   Ініціалізація пристроїв SmartLock в одній прошивці.
  ******************************************************************************
  */
#include "app.h"

#include "platform.h"

App_Devices app_devices;

static void devices_init(void)
{
  App_Devices *d = &app_devices;

  d->mpu_status   = mpu6050_init(&d->mpu, Platform_SensorI2c(), MPU6050_I2C_ADDR);
  d->baro_status  = bmp180_init(&d->baro, Platform_SensorI2c(), Platform_DelayMs, 0U);
  d->flash_status = w25q64_init(&d->flash, Platform_FlashSpi(), Platform_DelayMs);
}

void App_Init(void)
{
  Platform_BringUp();
  devices_init();
}

void App_Loop(void)
{
  Platform_Poll();
}
