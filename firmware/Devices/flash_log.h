/**
  ******************************************************************************
  * @file    flash_log.h
  * @brief   Енергонезалежний журнал подій SmartLock у W25Q64 (FR-08).
  *
  *          Формат і правила — docs/lab_3/flash-layout.md, розділи 2 і 3:
  *            - кільце з 15 секторів, записи по 32 Б з CRC-32;
  *            - запис лише дописується і ніколи не переписується;
  *            - перед першим записом у сектор він стирається (по колу це
  *              знищує 128 найстаріших записів);
  *            - після рестарту flash_log_mount() знаходить останній запис за
  *              наскрізним номером seq і продовжує після нього; пошкоджені
  *              записи (хибна CRC) пропускаються і рахуються.
  *
  *          Модуль працює лише через драйвер w25q64 і не знає про HAL.
  *          Операції блокувальні: запис ≤ 2 мс, стирання сектора — 64 мс
  *          (виміряно, до 400 мс за datasheet). Щоб стирання не збігалося із
  *          записом в одному проході циклу, його можна виконати заздалегідь:
  *          flash_log_needs_erase() → flash_log_prepare().
  ******************************************************************************
  */
#ifndef FLASH_LOG_H
#define FLASH_LOG_H

#include <stdbool.h>
#include <stdint.h>

#include "flash_layout.h"
#include "w25q64.h"

/* Коди подій (docs/lab_3/flash-layout.md, розділ 3.2) */
typedef enum
{
  FLASH_LOG_BOOT           = 0x01,
  FLASH_LOG_LOCK_ACTIVATED = 0x10,
  FLASH_LOG_LOCKED         = 0x11,
  FLASH_LOG_UNLOCKED       = 0x12,
  FLASH_LOG_ACCESS_GRANTED = 0x20,
  FLASH_LOG_WRONG_CODE     = 0x21,
  FLASH_LOG_MOTION         = 0x30,
  FLASH_LOG_TAMPER         = 0x31,
  FLASH_LOG_FORCED_OPEN    = 0x32,
  FLASH_LOG_DOOR_OPEN      = 0x40,
  FLASH_LOG_DOOR_CLOSED    = 0x41,
  FLASH_LOG_ENV_WARNING    = 0x50,
  FLASH_LOG_PHOTO_SAVED    = 0x60,
  FLASH_LOG_PHOTO_FAILED   = 0x61,
  FLASH_LOG_TEST           = 0x7F    /* тестовий запис стендової перевірки */
} flash_log_type_t;

#define FLASH_LOG_NO_FRAME  0xFFFFFFFFUL

/* Одна подія. seq і boot заповнює модуль під час flash_log_append(). */
typedef struct
{
  uint32_t seq;
  uint16_t boot;
  uint8_t  type;       /* flash_log_type_t */
  uint32_t t_ms;
  uint32_t arg;
  uint32_t arg2;
  uint32_t frame_id;   /* FLASH_LOG_NO_FRAME — кадру немає */
} flash_log_entry_t;

typedef enum
{
  FLASH_LOG_SLOT_EMPTY = 0,   /* 32 × 0xFF */
  FLASH_LOG_SLOT_VALID,
  FLASH_LOG_SLOT_DAMAGED      /* не порожній і не пройшов перевірку маркера / версії / CRC */
} flash_log_slot_t;

/* Чому під час монтування журнал відформатовано */
typedef enum
{
  FLASH_LOG_MOUNTED = 0,      /* розмітка збіглася, журнал відновлено */
  FLASH_LOG_FORMAT_BLANK,     /* службовий сектор порожній: перший запуск */
  FLASH_LOG_FORMAT_LAYOUT     /* службовий запис іншої розмітки або пошкоджений */
} flash_log_mount_t;

typedef struct
{
  w25q64_t         *dev;
  bool              mounted;
  flash_log_mount_t mount_result;

  uint32_t head;              /* номер вільного місця для наступного запису, 0..CAPACITY-1 */
  bool     head_needs_erase;  /* сектор head ще не стертий */
  uint32_t next_seq;
  uint16_t boot;              /* номер поточного запуску */

  uint8_t  valid[FLASH_LOG_SECTORS];     /* дійсних записів у секторі */
  uint8_t  damaged[FLASH_LOG_SECTORS];   /* пошкоджених записів у секторі */
} flash_log_t;

/* Ітератор від найстарішого запису до найновішого */
typedef struct
{
  uint32_t index;   /* поточне місце в кільці */
  uint32_t left;    /* скільки місць лишилося переглянути */
} flash_log_iter_t;

/* Перевірити службовий сектор, за потреби відформатувати, знайти останній
 * запис. dev має бути ініціалізований (w25q64_init). Сам запис BOOT не робить. */
dev_status_t flash_log_mount(flash_log_t *log, w25q64_t *dev);

/* Стерти журнал і службовий сектор, записати опис розмітки. seq і boot — з 1. */
dev_status_t flash_log_format(flash_log_t *log);

/* Чи потребує наступний запис стирання сектора (≈ 64 мс) */
bool flash_log_needs_erase(const flash_log_t *log);

/* Стерти сектор наступного запису заздалегідь, якщо це потрібно */
dev_status_t flash_log_prepare(flash_log_t *log);

/* Дописати подію і заповнити e->boot.
 * e->seq == 0 — модуль бере наступний номер (next_seq). Інакше це номер, який
 * подія вже отримала в журналі застосунку (sl_journal.c: ID у рядку EVT);
 * він має бути ≥ next_seq, пропуски дозволені (подія, відкинута до запису),
 * менший номер — DEV_ERR_PARAM. Так ID в UART і у Flash збігаються.
 * При помилці запису місце і номер вважаються використаними (запис міг
 * частково потрапити у Flash). */
dev_status_t flash_log_append(flash_log_t *log, flash_log_entry_t *e);

void flash_log_iter_begin(const flash_log_t *log, flash_log_iter_t *it);

/* Наступний непорожній запис. Повертає DEV_OK; *state = FLASH_LOG_SLOT_EMPTY
 * означає, що записів більше немає. addr — адреса запису у Flash (може бути NULL). */
dev_status_t flash_log_next(flash_log_t *log, flash_log_iter_t *it,
                            flash_log_entry_t *e, flash_log_slot_t *state, uint32_t *addr);

/* Підсумок: дійсні й пошкоджені записи в журналі */
uint32_t flash_log_valid_count(const flash_log_t *log);
uint32_t flash_log_damaged_count(const flash_log_t *log);

/* Адреса місця index у Flash (index береться за модулем CAPACITY) */
uint32_t flash_log_slot_addr(uint32_t index);

/* Розбір і формування 32 Б запису (для тестів і стендового виводу) */
void             flash_log_pack(const flash_log_entry_t *e, uint8_t out[FLASH_LOG_RECORD_SIZE]);
flash_log_slot_t flash_log_unpack(const uint8_t in[FLASH_LOG_RECORD_SIZE], flash_log_entry_t *e);

/* Назва коду події для UART ("MOTION"), "UNKNOWN" для невідомого */
const char *flash_log_type_name(uint8_t type);

#endif /* FLASH_LOG_H */
