/**
  ******************************************************************************
  * @file    ov7670.c
  * @brief   OV7670: PID/VER і конфігурація регістрів по SCCB.
  ******************************************************************************
  */
#include "ov7670.h"

/* Регістри таблиці QQVGA RGB565 (datasheet v1.4, табл. 5) */
#define REG_COM1              0x04U
#define REG_COM3              0x0CU   /* біт 2 DCW enable */
#define REG_COM14             0x3EU   /* PCLK divider + ручне масштабування */
#define REG_COM15             0x40U   /* формат RGB */
#define REG_RGB444            0x8CU
#define REG_HSTART            0x17U
#define REG_HSTOP             0x18U
#define REG_HREF              0x32U
#define REG_VSTART            0x19U
#define REG_VSTOP             0x1AU
#define REG_VREF              0x03U
#define REG_SCALING_DCWCTR    0x72U
#define REG_SCALING_PCLK_DIV  0x73U
/* Автоматика і колір */
#define REG_GAIN              0x00U
#define REG_BLUE              0x01U
#define REG_RED               0x02U
#define REG_COM4              0x0DU
#define REG_COM5              0x0EU
#define REG_COM6              0x0FU
#define REG_AECH              0x10U
#define REG_COM9              0x14U   /* стеля підсилення AGC */
#define REG_AEW               0x24U   /* межі AEC: верхня */
#define REG_AEB               0x25U   /*           нижня */
#define REG_VPT               0x26U   /* швидка зона AEC */
#define REG_COM11             0x3BU
#define REG_COM12             0x3CU
#define REG_EDGE              0x3FU
#define REG_COM16             0x41U
#define REG_CONTRAS           0x56U
#define REG_MTX1              0x4FU   /* кольорова матриця 0x4F..0x54, знаки — 0x58 */
#define REG_MTXS              0x58U
#define REG_GFIX              0x69U
#define REG_HAECC1            0x9FU   /* гістограмний AEC 0x9F, 0xA0, 0xA6..0xAA */
#define REG_BD50MAX           0xA5U
#define REG_BD60MAX           0xABU
#define COM8_AEC_PARAMS       0xE0U   /* FASTAEC|AECSTEP|BFILT, ще без AGC/AEC/AWB */
#define COM8_AEC_AGC          0xE5U   /* + AGC + AEC */

/* УВАГА: значення — поширений «рецепт» QQVGA з відкритих драйверів OV7670
 * (VGA + DCW /4 + PCLK /4, вікно HSTART..VREF), а не з таблиці виробника, якої
 * для QQVGA немає. Їх перевіряють у H14: CAM SNAP має дати рівно 120 ліній по
 * 320 байт (short_lines = 0), а кадр — розпізнавану сцену. Якщо розмір не
 * збігся, змінюється лише ця таблиця. */
static const ov7670_reg_t qqvga_rgb565[] =
{
  { OV7670_REG_COM7,      0x04U },   /* VGA, вихід RGB */
  { REG_RGB444,           0x00U },   /* RGB444 вимкнено */
  { REG_COM1,             0x00U },
  { REG_COM15,            0xD0U },   /* RGB565, повний діапазон 00..FF */
  { REG_COM3,             0x04U },   /* DCW (downsample) увімкнено */
  { REG_COM14,            0x1AU },   /* ручне масштабування, PCLK / 4 */
  { REG_SCALING_DCWCTR,   0x22U },   /* зменшення /4 по горизонталі і вертикалі */
  { REG_SCALING_PCLK_DIV, 0xF2U },   /* PCLK / 4 для DSP */
  { REG_HSTART,           0x16U },
  { REG_HSTOP,            0x04U },
  { REG_HREF,             0xA4U },
  { REG_VSTART,           0x02U },
  { REG_VSTOP,            0x7AU },
  { REG_VREF,             0x0AU },

  /* ---- ЛР3: автоматика і колір (виправлення пересвічення, лог 23) ----------------
   * Без цього блоку датчик після скидання працює з фіксованою експозицією і без
   * балансу білого: кадр пересвічений, кадр TAMPER майже білий (FR-07 «з обмеженням»).
   * Значення — з драйвера Linux drivers/media/i2c/ov7670.c (ov7670_default_regs і
   * ov7670_fmt_rgb565), БЕЗ регістрів, що міняють такти, вікно чи формат:
   *   DBLV 0x6B (PLL ×4 — PCLK став би вчетверо швидшим за опитування GPIO), TSLB 0x3A
   *   і COM13[0] (порядок байтів), MVFP 0x1E, COM10, 0x70..0x73, 0xA2, NT_CTRL 0xA4
   *   (авто-зниження частоти кадрів змінило б період VSYNC).
   * Підбір — у H14 (docs/camera.md §7), команда CAM REG без перепрошивки. */

  /* Гамма-крива */
  { 0x7AU, 0x20U }, { 0x7BU, 0x10U }, { 0x7CU, 0x1EU }, { 0x7DU, 0x35U },
  { 0x7EU, 0x5AU }, { 0x7FU, 0x69U }, { 0x80U, 0x76U }, { 0x81U, 0x80U },
  { 0x82U, 0x88U }, { 0x83U, 0x8FU }, { 0x84U, 0x96U }, { 0x85U, 0xA3U },
  { 0x86U, 0xAFU }, { 0x87U, 0xC4U }, { 0x88U, 0xD7U }, { 0x89U, 0xE8U },

  /* AEC / AGC: спершу параметри при вимкненій автоматиці, потім увімкнення */
  { OV7670_REG_COM8,      COM8_AEC_PARAMS },
  { REG_GAIN,             0x00U },
  { REG_AECH,             0x00U },
  { REG_COM4,             0x40U },
  { REG_COM9,             0x18U },   /* стеля підсилення ×4: менше шуму в темряві */
  { REG_BD50MAX,          0x05U },
  { REG_BD60MAX,          0x07U },
  { REG_AEW,              0x95U },   /* цільова яскравість: верхня межа */
  { REG_AEB,              0x33U },   /*                     нижня межа */
  { REG_VPT,              0xE3U },
  { REG_HAECC1,           0x78U }, { 0xA0U, 0x68U }, { 0xA1U, 0x03U },
  { 0xA6U,                0xD8U }, { 0xA7U, 0xD8U }, { 0xA8U, 0xF0U },
  { 0xA9U,                0x90U }, { 0xAAU, 0x94U },
  { OV7670_REG_COM8,      COM8_AEC_AGC },

  /* Аналогові «reserved» значення з того ж драйвера */
  { REG_COM5,             0x61U },
  { REG_COM6,             0x4BU },
  { 0x16U,                0x02U },
  { 0x21U,                0x02U }, { 0x22U, 0x91U }, { 0x29U, 0x07U },
  { 0x33U,                0x0BU }, { 0x35U, 0x0BU }, { 0x37U, 0x1DU },
  { 0x38U,                0x71U }, { 0x39U, 0x2AU },
  { REG_COM12,            0x78U },
  { 0x4DU,                0x40U }, { 0x4EU, 0x20U },
  { REG_GFIX,             0x00U },
  { 0x74U,                0x10U },
  { 0x8DU,                0x4FU }, { 0x8EU, 0x00U }, { 0x8FU, 0x00U }, { 0x90U, 0x00U },
  { 0x91U,                0x00U }, { 0x96U, 0x00U }, { 0x9AU, 0x00U },
  { 0xB0U,                0x84U }, { 0xB1U, 0x0CU }, { 0xB2U, 0x0EU },
  { 0xB3U,                0x82U }, { 0xB8U, 0x0AU },

  /* AWB: параметри, стартові підсилення R/B, увімкнення */
  { 0x43U,                0x0AU }, { 0x44U, 0xF0U }, { 0x45U, 0x34U },
  { 0x46U,                0x58U }, { 0x47U, 0x28U }, { 0x48U, 0x3AU },
  { 0x59U,                0x88U }, { 0x5AU, 0x88U }, { 0x5BU, 0x44U },
  { 0x5CU,                0x67U }, { 0x5DU, 0x49U }, { 0x5EU, 0x0EU },
  { 0x6CU,                0x0AU }, { 0x6DU, 0x55U }, { 0x6EU, 0x11U },
  { 0x6FU,                0x9FU }, { 0x6AU, 0x40U },
  { REG_BLUE,             0x40U },
  { REG_RED,              0x60U },
  { OV7670_REG_COM8,      OV7670_COM8_AUTO },

  /* Кольорова матриця для RGB (ov7670_fmt_rgb565) */
  { REG_MTX1,             0xB3U }, { 0x50U, 0xB3U }, { 0x51U, 0x00U },
  { 0x52U,                0x3DU }, { 0x53U, 0xA7U }, { 0x54U, 0xE4U },
  { REG_MTXS,             0x9EU },

  /* Різкість, шумозаглушення, контраст, гамма */
  { REG_EDGE,             0x00U },
  { 0x75U,                0x05U }, { 0x76U, 0xE1U }, { 0x4CU, 0x00U }, { 0x77U, 0x01U },
  { 0x4BU,                0x09U }, { 0xC9U, 0x60U },
  { REG_COM16,            0x38U },   /* AWB-підсилення, авто-різкість і шумозаглушення */
  { REG_CONTRAS,          0x40U },
  { 0x34U,                0x11U },
  { REG_COM11,            0x12U },   /* авто 50/60 Гц, експозиція коротша за смугу */
  { OV7670_REG_COM13,     0xC0U },   /* гамма + авто-насиченість UV; біт 0 = 0 */
};

const ov7670_reg_t *ov7670_qqvga_rgb565_table(size_t *count)
{
  if (count != NULL)
  {
    *count = sizeof(qqvga_rgb565) / sizeof(qqvga_rgb565[0]);
  }
  return qqvga_rgb565;
}

void ov7670_rgb565_swap_bytes(uint8_t *buf, size_t len)
{
  if (buf == NULL)
  {
    return;
  }
  for (size_t i = 0U; (i + 1U) < len; i += 2U)
  {
    uint8_t t  = buf[i];
    buf[i]     = buf[i + 1U];
    buf[i + 1U] = t;
  }
}

dev_status_t ov7670_read_reg(const ov7670_t *dev, uint8_t reg, uint8_t *val)
{
  if ((dev == NULL) || (dev->read == NULL) || (val == NULL))
  {
    return DEV_ERR_PARAM;
  }
  return dev->read(dev->addr7, reg, val);
}

dev_status_t ov7670_write_reg(const ov7670_t *dev, uint8_t reg, uint8_t val)
{
  if ((dev == NULL) || (dev->write == NULL))
  {
    return DEV_ERR_PARAM;
  }
  return dev->write(dev->addr7, reg, val);
}

dev_status_t ov7670_init(ov7670_t *dev, ov7670_sccb_read_fn read, ov7670_sccb_write_fn write,
                         dev_delay_ms_fn delay_ms)
{
  dev_status_t st;

  if ((dev == NULL) || (read == NULL) || (write == NULL))
  {
    return DEV_ERR_PARAM;
  }
  dev->read     = read;
  dev->write    = write;
  dev->delay_ms = delay_ms;
  dev->addr7    = OV7670_I2C_ADDR;
  dev->pid      = 0U;
  dev->ver      = 0U;

  st = ov7670_read_reg(dev, OV7670_REG_PID, &dev->pid);
  if (st == DEV_OK)
  {
    st = ov7670_read_reg(dev, OV7670_REG_VER, &dev->ver);
  }
  if ((st == DEV_OK) && ((dev->pid != OV7670_PID_VALUE) || (dev->ver != OV7670_VER_VALUE)))
  {
    st = DEV_ERR_ID;
  }
  return st;
}

dev_status_t ov7670_configure(const ov7670_t *dev, const ov7670_reg_t *table, size_t count,
                              uint8_t clkrc, uint8_t *failed_reg)
{
  dev_status_t st;

  if (failed_reg != NULL)
  {
    *failed_reg = 0U;
  }
  if ((dev == NULL) || ((table == NULL) && (count != 0U)))
  {
    return DEV_ERR_PARAM;
  }

  st = ov7670_write_reg(dev, OV7670_REG_COM7, OV7670_COM7_RESET);
  if (st != DEV_OK)
  {
    if (failed_reg != NULL)
    {
      *failed_reg = OV7670_REG_COM7;
    }
    return st;
  }
  if (dev->delay_ms != NULL)
  {
    dev->delay_ms(OV7670_RESET_DELAY_MS);
  }

  st = ov7670_write_reg(dev, OV7670_REG_CLKRC, (uint8_t)(clkrc & OV7670_CLKRC_MASK));
  if (st != DEV_OK)
  {
    if (failed_reg != NULL)
    {
      *failed_reg = OV7670_REG_CLKRC;
    }
    return st;
  }

  for (size_t i = 0U; i < count; i++)
  {
    st = ov7670_write_reg(dev, table[i].reg, table[i].val);
    if (st != DEV_OK)
    {
      if (failed_reg != NULL)
      {
        *failed_reg = table[i].reg;
      }
      return st;
    }
  }
  return DEV_OK;
}
