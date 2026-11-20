/**
  ******************************************************************************
  * @file    ov7670.h
  * @brief   OV7670: ідентифікація і конфігурація регістрів по SCCB (ЛР3, I07).
  *
  *          Захоплення пікселів сюди не входить: воно потребує прямого читання
  *          GPIO на кожному фронті PCLK і живе в Platform/camera_dvp.c.
  *
  *          SCCB (OmniVision Serial Camera Control Bus Functional Spec.) вимагає
  *          STOP між фазою запису номера регістру і фазою читання (2-phase write +
  *          2-phase read), тому драйвер не використовує dev_i2c_bus_t::read_reg
  *          (там repeated START), а отримує дві функції — на платі це
  *          Platform_SccbRead / Platform_SccbWrite з ЛР2.
  *
  *          Джерела: OV7670/OV7171 CMOS VGA Camera Chip Datasheet v1.4 (табл. 5
  *          «Device Control Register List»: PID 0x0A, VER 0x0B, CLKRC 0x11,
  *          COM7 0x12, COM3 0x0C, COM14 0x3E, COM15 0x40, 0x70–0x73, 0xA2).
  ******************************************************************************
  */
#ifndef OV7670_H
#define OV7670_H

#include <stddef.h>
#include <stdint.h>

#include "dev_bus.h"

#define OV7670_I2C_ADDR       0x21U   /* 7-бітна (0x42 / 0x43 на шині) */
#define OV7670_PID_VALUE      0x76U
#define OV7670_VER_VALUE      0x73U

#define OV7670_REG_PID        0x0AU
#define OV7670_REG_VER        0x0BU
#define OV7670_REG_CLKRC      0x11U
#define OV7670_REG_COM7       0x12U
#define OV7670_COM7_RESET     0x80U   /* скидання всіх регістрів до заводських */
#define OV7670_CLKRC_MASK     0x3FU   /* CLKRC[5:0]: f_int = XCLK / (CLKRC + 1) */
#define OV7670_REG_COM8       0x13U   /* автоматика: AEC, AGC, AWB */
#define OV7670_COM8_AUTO      0xE7U   /* FASTAEC|AECSTEP|BFILT|AGC|AWB|AEC */
#define OV7670_REG_COM13      0x3DU   /* гамма, насиченість UV; біт 0 — порядок байтів */
#define OV7670_REG_TSLB       0x3AU   /* біт 3 — порядок байтів (разом з COM13[0]) */

/* Формат, який дає ov7670_qqvga_rgb565_table(): 160x120, RGB565, 2 байти/піксель */
#define OV7670_QQVGA_WIDTH    160U
#define OV7670_QQVGA_HEIGHT   120U
#define OV7670_RGB565_BPP     2U

/* Час після програмного скидання до першого запису. Datasheet не дає числа для
 * COM7[7]; 30 мс — з запасом, вимірюється разом з ініціалізацією в H14. */
#define OV7670_RESET_DELAY_MS 30U

typedef dev_status_t (*ov7670_sccb_read_fn)(uint8_t addr7, uint8_t reg, uint8_t *val);
typedef dev_status_t (*ov7670_sccb_write_fn)(uint8_t addr7, uint8_t reg, uint8_t val);

typedef struct
{
  uint8_t reg;
  uint8_t val;
} ov7670_reg_t;

typedef struct
{
  ov7670_sccb_read_fn  read;
  ov7670_sccb_write_fn write;
  dev_delay_ms_fn      delay_ms;
  uint8_t              addr7;
  uint8_t              pid;
  uint8_t              ver;
} ov7670_t;

/* Зчитує PID/VER. DEV_ERR_ID — відповідає, але це не OV7670 (76 / 73). */
dev_status_t ov7670_init(ov7670_t *dev, ov7670_sccb_read_fn read, ov7670_sccb_write_fn write,
                         dev_delay_ms_fn delay_ms);

dev_status_t ov7670_read_reg(const ov7670_t *dev, uint8_t reg, uint8_t *val);
dev_status_t ov7670_write_reg(const ov7670_t *dev, uint8_t reg, uint8_t val);

/* Програмне скидання (COM7 = 0x80), пауза, CLKRC = clkrc & 0x3F, далі таблиця
 * по порядку. *failed_reg (може бути NULL) — регістр, на якому зупинились. */
dev_status_t ov7670_configure(const ov7670_t *dev, const ov7670_reg_t *table, size_t count,
                              uint8_t clkrc, uint8_t *failed_reg);

/* QQVGA 160x120 RGB565: VGA-вікно, зменшене DCW у 4 рази по X і Y, і (ЛР3, виправлення
 * пересвічення) автоматика AEC/AGC/AWB, гамма і кольорова матриця RGB. */
const ov7670_reg_t *ov7670_qqvga_rgb565_table(size_t *count);

/* RGB565 з датчика → «старший байт першим» (формат кадру в прошивці, у W25Q64 і в
 * tools/frame_decode.py). З цією таблицею (TSLB і COM13[0] за замовчуванням) OV7670
 * віддає молодший байт пікселя першим (GGGBBBBB, RRRRRGGG) — стенд, лог 23 ЛР3:
 * без перестановки — «райдужні» смуги, з перестановкою — плавна сцена.
 * Міняє місцями байти кожної пари; len — парна кількість байтів. */
void ov7670_rgb565_swap_bytes(uint8_t *buf, size_t len);

#endif /* OV7670_H */
