/**
  ******************************************************************************
  * @file    storage.h
  * @brief   Журнал подій SmartLock у W25Q64 (FR-08): бекенд для sl_journal.c.
  *
  *          Записи формує журнал застосунку (sl_journal.c): SmartLock_LogEvent()
  *          ставить запис у RAM-чергу і дає йому ID, задача storage за прохід
  *          забирає один запис, друкує рядок EVT і викликає
  *          SmartLock_JournalOps()->append(). Цей модуль визначає сильну
  *          SmartLock_JournalOps(): append() пише запис у W25Q64 через
  *          Devices/flash_log.c (32 Б, CRC-32, кільце з 15 секторів).
  *
  *          ID запису (seq) у Flash той самий, що в рядку EVT: Storage_Init()
  *          після монтування передає останній seq з Flash у
  *          SmartLock_JournalSetLastSeq(), тож після рестарту нумерація
  *          продовжується, а не починається з 1.
  *
  *          Тривалість append(): запис 32 Б і зчитування назад ≤ 2 мс; один раз на
  *          128 записів перед записом стирається сектор (виміряно 64 мс, до 400 мс
  *          за datasheet) — docs/tasks.md, задача storage.
  *
  *          Формат і правила — docs/lab_3/flash-layout.md, розділи 2 і 3.
  ******************************************************************************
  */
#ifndef STORAGE_H
#define STORAGE_H

#include <stdbool.h>
#include <stdint.h>

#include "flash_log.h"
#include "sl_journal.h"

/* Невдалих стирань поспіль, після яких журнал вимикається, щоб кожен запис не
 * блокував цикл на тайм-ауті стирання. PRD і datasheet не задають — TBD. */
#ifndef TBD_STORAGE_ERASE_RETRIES
#define TBD_STORAGE_ERASE_RETRIES 3U
#endif

/* Змонтувати журнал (≈ 0,2 с; при першому запуску — форматування ≈ 1 с),
 * записати BOOT, продовжити нумерацію sl_journal і надрукувати LOG,MOUNT,...
 * Викликається з App_Init() після ініціалізації W25Q64 і до старту задач;
 * якщо флешка не відповіла — журнал вимкнений, append() повертає помилку. */
void Storage_Init(void);

/* Журнал змонтовано і ним можна користуватися */
bool Storage_Ready(void);

/* Код події у Flash (flash-layout.md, розділ 3.2) для типу sl_journal.
 * 0 — для такого типу запису у Flash немає. */
uint8_t Storage_FlashType(sl_log_type_t type);

/* Записати запис журналу застосунку у Flash: те, що викликає
 * SmartLock_JournalOps()->append() (storage_ops.c). За потреби спершу стирає
 * сектор. DEV_ERR_STATE — журнал вимкнений, DEV_ERR_PARAM — тип без коду у
 * Flash або seq менший за вже записаний. */
dev_status_t Storage_Append(const sl_log_rec_t *rec);

/* Записати запис прямо у Flash, поза чергою журналу застосунку (стендові
 * команди: тестова подія і PHOTO_SAVED / PHOTO_FAILED для кадру, команда p).
 * type — код Flash (flash_log_type_t). seq (може бути NULL) — номер запису,
 * 0, якщо журнал вимкнений. Друкує LOG,seq=... або LOG,ERR,...; нумерація
 * sl_journal після запису йде далі. */
dev_status_t Storage_WriteRecord(uint8_t type, uint32_t arg, uint32_t arg2, uint32_t frame_id,
                                 uint32_t *seq);

/* Помилок стирання / запису у Flash від старту */
uint32_t Storage_Errors(void);

/* Надрукувати весь журнал від найстарішого запису і рядок LOG,SUMMARY */
void Storage_Dump(void);

/* Стан журналу для стендових команд і модуля кадрів */
flash_log_t *Storage_Journal(void);

/* Однолітерні стендові команди журналу (d, e, z). true — команду оброблено. */
bool Storage_BenchCommand(int c);
void Storage_BenchHelp(void);

/* Рядкові команди (cmd.c): "LOG ERASE". true — команду оброблено. */
bool Storage_LineCommand(const char *line);

/* Перші 8 шістнадцяткових цифр git-хешу збірки як число (для запису BOOT) */
uint32_t Storage_HashWord(const char *git_hash);

#endif /* STORAGE_H */
