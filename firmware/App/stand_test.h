/**
  ******************************************************************************
  * @file    stand_test.h
  * @brief   Перевірки пристроїв стенда ЛР2 для UART self-test.
  *
  *          Кожна перевірка звертається до заліза лише через platform.h і
  *          драйвери Devices/ і заповнює self_test_device_t разом із буфером
  *          полів "ключ=значення,...". Сама перевірка нічого не друкує:
  *          рядки формує і виводить модуль self_test.c.
  *
  *          Для FAIL код причини береться з поля error (err=<код> додає
  *          SelfTest_FormatLine()), тому в полях err= не повторюється.
  *          Для SKIP err= є частиною полів.
  ******************************************************************************
  */
#ifndef STAND_TEST_H
#define STAND_TEST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "dev_bus.h"
#include "self_test.h"

/* Розмір буфера полів одного пристрою (найдовший — LSM303D, ~110 символів) */
#define STAND_TEST_FIELDS_MAX 160U

/* Кількість перевірок у повному прогоні StandTest_Run() */
#define STAND_TEST_DEVICE_COUNT 10U

/* Перевірка одного пристрою: заповнює device, поля пише у fields */
typedef void (*stand_test_check_fn)(
    self_test_device_t *device,
    char *fields,
    size_t size
);

/* ---- допоміжне форматування (лише libc) ---------------------------------- */

/* Код помилки для виводу команд: "ok" для DEV_OK, інакше як у полі err= */
const char *StandTest_ErrStr(dev_status_t status);

/* Число з фіксованою комою v / 10^dec: (207, 1) -> "20.7", (-5, 3) -> "-0.005".
 * Повертає довжину, як snprintf. */
int StandTest_FormatFixed(
    char *buffer,
    size_t size,
    int32_t value,
    unsigned dec
);

/* Знімок входів: "btn=1111,lock_b1=1,door=1,pir=0,mpu_int=0" (біти PLATFORM_IN_*) */
int StandTest_FormatInputs(
    char *buffer,
    size_t size,
    uint32_t inputs
);

/* ---- перевірки пристроїв ------------------------------------------------- */

/* UART: baud=115200 (якщо рядок видно — канал працює) */
void StandTest_CheckUart(self_test_device_t *device, char *fields, size_t size);

/* MPU-6050: при помилці шини — SKIP (модуль GY-521 несправний, FR-06 на LSM303D) */
void StandTest_CheckMpu(self_test_device_t *device, char *fields, size_t size);

/* BMP180: id, t_c, p_hpa; поза кімнатним діапазоном — WARN */
void StandTest_CheckBmp(self_test_device_t *device, char *fields, size_t size);

/* L3GD20: WHO_AM_I на 0x6B, потім на 0x6A */
void StandTest_CheckL3gd20(self_test_device_t *device, char *fields, size_t size);

/* LSM303D (FR-06): |a| у спокої 1 g ± 5 %, інакше WARN */
void StandTest_CheckAccel(self_test_device_t *device, char *fields, size_t size);

/* W25Q64: JEDEC ID і ємність */
void StandTest_CheckFlash(self_test_device_t *device, char *fields, size_t size);

/* OV7670: PID/VER по SCCB і частота VSYNC за 1 с */
void StandTest_CheckCamera(self_test_device_t *device, char *fields, size_t size);

/* DHT11: температура, вологість, сирий кадр; помилка CRC — FAIL з hint=crc */
void StandTest_CheckDht11(self_test_device_t *device, char *fields, size_t size);

/* ШІМ серво: 50 Гц, 1000..2000 мкс (зворотного зв'язку немає) */
void StandTest_CheckServo(self_test_device_t *device, char *fields, size_t size);

/* Знімок цифрових входів (кнопки, геркон, PIR, INT MPU) */
void StandTest_CheckGpio(self_test_device_t *device, char *fields, size_t size);

/* ---- вивід --------------------------------------------------------------- */

/* Інформаційний рядок "I2C,scan,0x..,..." (у підсумок не входить) */
void StandTest_I2cScan(void);

/* Один рядок "ST,<пристрій>,..." через вивід self_test.c */
bool StandTest_PrintOne(const self_test_device_t *device);

/* Виконує перевірку check і виводить її рядок; повертає її стан */
self_test_state_t StandTest_RunOne(stand_test_check_fn check);

/* Повний прогін: заголовок, скан I2C, усі перевірки і SelfTest_Report()
 * (ST,START, рядки пристроїв, ST,SUMMARY). true — усе OK. */
bool StandTest_Run(void);

#endif /* STAND_TEST_H */
