/**
  ******************************************************************************
  * @file    sl_camera_link.c
  * @brief   Слабка заглушка SmartLock_CameraRequest() (див. sl_camera_link.h).
  *
  *          Камери ще немає (I07–I09): запит не приймається. Модуль камери
  *          визначить сильну функцію з тим самим ім'ям (I10).
  ******************************************************************************
  */
#include "sl_camera_link.h"

__attribute__((weak)) bool SmartLock_CameraRequest(uint32_t event_id)
{
    (void)event_id;
    return false;
}
