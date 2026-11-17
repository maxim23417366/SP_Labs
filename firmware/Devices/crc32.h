/**
  ******************************************************************************
  * @file    crc32.h
  * @brief   CRC-32 (IEEE 802.3, як у ZIP і Ethernet) для журналу і кадрів у W25Q64.
  *
  *          Поліном 0x04C11DB7 у відбитій формі 0xEDB88320, початкове значення
  *          0xFFFFFFFF, фінальний XOR 0xFFFFFFFF. Контрольне значення для ASCII
  *          "123456789" — 0xCBF43926. Опис формату — docs/lab_3/flash-layout.md.
  *
  *          Рахується частинами: crc = crc32_update(CRC32_INIT, a, n1);
  *          crc = crc32_update(crc, b, n2); результат = crc32_final(crc).
  ******************************************************************************
  */
#ifndef CRC32_H
#define CRC32_H

#include <stddef.h>
#include <stdint.h>

#define CRC32_INIT 0xFFFFFFFFUL

/* Продовжити обчислення ще на len байтів (data може бути NULL лише при len = 0) */
uint32_t crc32_update(uint32_t crc, const void *data, size_t len);

/* Завершити обчислення: фінальний XOR */
uint32_t crc32_final(uint32_t crc);

/* CRC-32 одного буфера цілком */
uint32_t crc32_compute(const void *data, size_t len);

#endif /* CRC32_H */
