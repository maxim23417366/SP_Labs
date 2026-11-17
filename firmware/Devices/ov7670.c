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
};

const ov7670_reg_t *ov7670_qqvga_rgb565_table(size_t *count)
{
  if (count != NULL)
  {
    *count = sizeof(qqvga_rgb565) / sizeof(qqvga_rgb565[0]);
  }
  return qqvga_rgb565;
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
