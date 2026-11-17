/**
  * @file    fake_w25q64.h
  * @brief   Fake SPI-шини з моделлю W25Q64 у RAM: справжній драйвер w25q64 працює
  *          з нею так само, як зі стендовою мікросхемою.
  *
  *          На відміну від mock_spi, тут є модель пам'яті 8 МБ з правилами NOR:
  *            - після стирання сектора 4 КБ усі байти = 0xFF;
  *            - Page Program лише скидає біти (новий = старий & записаний) і
  *              переходить по колу в межах сторінки 256 Б;
  *            - 02h / 20h виконуються лише після 06h (WEL), після них WEL = 0.
  *          Підтримані команди: 06h, 05h, 02h, 03h, 20h, 9Fh. BUSY завжди 0.
  *
  *          Обрив живлення: fake_w25q64_cut_after(n) — наступні n байтів, які
  *          змінюють записи та стирання, застосовуються, а потім мікросхема
  *          «вимикається»: решта операції і всі наступні записи/стирання
  *          ігноруються, доки тест не викличе fake_w25q64_power_on().
  */
#ifndef FAKE_W25Q64_H
#define FAKE_W25Q64_H

#include "dev_bus.h"

#define FAKE_W25Q64_SIZE (8UL * 1024UL * 1024UL)

/* Уся пам'ять 0xFF, лічильники нулі, живлення ввімкнене */
void fake_w25q64_reset(void);

const dev_spi_bus_t *fake_w25q64_bus(void);

/* Пряма робота з пам'яттю для тестів: підготовка вмісту і перевірка */
uint8_t *fake_w25q64_mem(void);

/* Обрив живлення після n змінених байтів (запис — байти даних, стирання — 4096) */
void fake_w25q64_cut_after(uint32_t bytes);
void fake_w25q64_power_on(void);
int  fake_w25q64_powered(void);

/* Наступні count викликів transfer() повертають помилку status */
void fake_w25q64_fail_transfers(unsigned count, dev_status_t status);

/* Те саме, але після skip успішних викликів transfer() */
void fake_w25q64_fail_from(unsigned skip, unsigned count, dev_status_t status);

/* Скільки разів виконано стирання сектора і запис сторінки */
unsigned fake_w25q64_erase_count(void);
unsigned fake_w25q64_program_count(void);
unsigned fake_w25q64_sector_erases(uint32_t sector);

/* transfer() без select(), повторний select() тощо */
unsigned fake_w25q64_protocol_errors(void);

#endif /* FAKE_W25Q64_H */
