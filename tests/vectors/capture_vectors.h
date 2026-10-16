/**
  * @file    capture_vectors.h
  * @brief   Тестові вектори з декодованих записів логічного аналізатора.
  *
  *          Кожен вектор — байти рівно в тому порядку, як їх показує декодер
  *          (адресний байт уже зсунутий і містить біт R/W), плюс результат
  *          ACK/NACK і посилання на файл запису. Драйвер, запущений на fake-шині,
  *          має відправити ті самі байти й отримати ті самі значення, що й
  *          прошивка в UART-лозі (вимога 4.6 ЛР2).
  *
  *          Новий вектор: декодувати запис у PulseView або
  *          sigrok-cli -i docs/captures/<file>.sr -P i2c:scl=D0:sda=D1 -A i2c
  *          і перенести адреси/дані сюди разом з іменем файлу та параметрами.
  */
#ifndef CAPTURE_VECTORS_H
#define CAPTURE_VECTORS_H

#include <stdint.h>

/* ЛР1, docs/screenshots/i2c-logic-analyzer.png — Saleae Logic 2, D0 = SCL (PB6),
 * D1 = SDA (PB7). SCL 100 кГц (період 10 мкс). Прошивка: PeriphCheck_Loop()
 * опитує MPU-6050, модуль не підключений. Декодер: "Setup Write to [0x68] + NAK".
 * Ручне декодування SDA на фронтах SCL: 1 1 0 1 0 0 0 | 0 (W) | 1 (NAK) = 0xD0.
 * Піни — розкладка ЛР1; у ЛР2 I2C1 переноситься на PB8/PB9 (docs/pinout.md),
 * але байти на шині від пінів не залежать, тому вектор лишається валідним. */
static const uint8_t cap_lr1_mpu6050_probe_wire[] = { 0xD0 };
#define CAP_LR1_MPU6050_PROBE_ACKED  0

#endif /* CAPTURE_VECTORS_H */
