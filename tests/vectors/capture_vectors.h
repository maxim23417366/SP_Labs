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

/* ЛР2, запис dht11_read.sr (напрям 4, docs/captures/) — sigrok, fs = 1 МГц
 * (роздільність 1 мкс), 1 000 000 відліків, канал CH7 = DHT11 DATA (PB6, D10).
 * Прошивка: команда h (bench.c, dht_read). Старт від МК — LOW 20 277 мкс,
 * відповідь датчика LOW 81 / HIGH 85 мкс; далі 40 бітів: LOW 56–57 мкс,
 * HIGH 23–25 мкс (0) або 72–73 мкс (1).
 * Тривалості HIGH витягнуто: python3 scripts/sr_dht11_bits.py dht11_read.sr
 * Кадр: 23 02 15 04 3E, сума 0x23+0x02+0x15+0x04 = 0x3E — OK
 * (35.2 %RH, 21.4 °C). */
static const uint16_t cap_dht11_read_high_us[40] = {
  23U, 23U, 72U, 24U, 23U, 23U, 72U, 73U,  /* байт 0: 0x23 */
  23U, 24U, 24U, 23U, 23U, 24U, 72U, 25U,  /* байт 1: 0x02 */
  23U, 23U, 24U, 72U, 24U, 72U, 23U, 72U,  /* байт 2: 0x15 */
  23U, 24U, 24U, 23U, 24U, 72U, 24U, 24U,  /* байт 3: 0x04 */
  23U, 24U, 72U, 72U, 72U, 72U, 72U, 24U,  /* байт 4: 0x3E */
};
static const uint8_t cap_dht11_read_frame[5] = { 0x23, 0x02, 0x15, 0x04, 0x3E };

/* ---- ЛР2, uart_selftest.sr: self-test (I2C + UART того самого прогону) -------------
 * sigrok, fs = 1 МГц (1 відлік = 1 мкс), 5 000 000 відліків; канали SCL (D0), SDA (D1),
 * TX (D6, USART2 PA2). Прошивка: SelfTest_Run() (раніше Bench_SelfTest), записано
 * 27.10.2026. Час — від початку запису, мс. Декодування:
 *   sigrok-cli -i docs/captures/uart_selftest.sr -P i2c:scl=SCL:sda=SDA -A i2c
 *   sigrok-cli -i docs/captures/uart_selftest.sr -P uart:rx=TX:baudrate=115200 -B uart=rx
 * Ініціалізація драйверів (калібрування BMP180 0xAA..0xBF, WHO_AM_I і CTRL1 LSM303D)
 * відбулася в App_Init() до запису і в трейс не потрапила: self-test лише читає.
 * SPI (JEDEC) у цьому записі не декодується: SCK 3 МГц > fs / 2. */

/* 520.872 мс: проба MPU-6050 — "Address write: 68", NACK (GY-521 несправний).
 * UART 521.015 мс: ST,MPU6050,SKIP,err=nack,... */
static const uint8_t cap_st_mpu6050_probe_wire[] = { 0xD0 };

/* BMP180 (0x77). UART 539.120 мс: ST,BMP180,OK,id=0x55,t_c=20.7,p_hpa=1002.73 */
static const uint8_t cap_st_bmp180_ut_cmd_wire[]  = { 0xEE, 0xF4, 0x2E };              /* 526.833 мс */
static const uint8_t cap_st_bmp180_ut_read_wire[] = { 0xEE, 0xF6, 0xEF, 0x55, 0x91 };  /* 532.479 мс */
static const uint8_t cap_st_bmp180_up_cmd_wire[]  = { 0xEE, 0xF4, 0x34 };              /* 532.967 мс, oss = 0 */
static const uint8_t cap_st_bmp180_up_read_wire[] = { 0xEE, 0xF6, 0xEF, 0xA8, 0x02, 0x00 }; /* 538.479 мс */
#define CAP_ST_BMP180_UT  0x5591   /* 21905 */
#define CAP_ST_BMP180_UP  0xA802   /* (A8 02 00) >> (8 − oss) = 43010 */
#define CAP_ST_BMP180_T_DC  207     /* з UART: t_c=20.7 */
#define CAP_ST_BMP180_P_PA  100273  /* з UART: p_hpa=1002.73 */

/* 543.407 мс: L3GD20 (0x6B) WHO_AM_I 0x0F -> D4.
 * UART 543.846 мс: ST,L3GD20,OK,addr=0x6B,who=0xD4 */
static const uint8_t cap_st_l3gd20_who_wire[] = { 0xD6, 0x0F, 0xD7, 0xD4 };

/* 546.987 мс: LSM303D (0x1D) читання OUT_X_L_A 0x28 | 0x80 (автоінкремент), 6 байт.
 * UART 547.989 мс: ST,LSM303D,WARN,addr=0x1D,who=0x49,role=FR06_accel,
 *                  raw=7FF8,7FF8,33CD,a_g=2.938,ax_g=1.998,ay_g=1.998,az_g=0.808 */
static const uint8_t cap_st_lsm303d_xyz_wire[] = { 0x3A, 0xA8, 0x3B, 0xF8, 0x7F, 0xF8, 0x7F, 0xCD, 0x33 };
#define CAP_ST_LSM303D_XYZ_OFFSET 3U   /* дані після 3A A8 3B */

/* OV7670 (SCCB, 0x21): запис номера регістру і читання — окремі транзакції зі STOP.
 * 562.772 мс W 0A; 562.973 мс R 76; 563.175 мс W 0B; 563.377 мс R 73.
 * UART 1564.532 мс (після 1 с підрахунку VSYNC): ST,OV7670,OK,pid=0x76,ver=0x73,vsync_hz=30 */
#define CAP_ST_OV7670_PID  0x76U
#define CAP_ST_OV7670_VER  0x73U

/* DHT11 у цьому записі на аналізатор не потрапив (D6 зайнятий TX); кадр — із самого
 * UART-рядка 1794.514 мс: raw=2209150545 (t_c=21.5; rh=34 — прошивка до коміту
 * 79d2c32, поточна для тих самих байтів друкує rh=34.9). */
static const uint8_t cap_st_dht11_frame[5] = { 0x22, 0x09, 0x15, 0x05, 0x45 };

/* UART-вивід self-test, рядок за рядком без "\r\n" (декодер uart, 115200 8N1).
 * Перед заголовком — порожній рядок "\r\n" (500.009 мс). */
static const char *const cap_st_uart_lines[] = {
  "# SmartLock selftest v2 (LR2)",                                                  /*  500.209 мс */
  "ST,UART,OK,baud=115200",                                                         /*  503.180 мс */
  "I2C,scan,0x1D,0x21,0x6B,0x77",                                                   /*  518.018 мс */
  "ST,MPU6050,SKIP,err=nack,note=GY-521_faulty_FR06_on_LSM303D",                    /*  521.015 мс */
  "ST,BMP180,OK,id=0x55,t_c=20.7,p_hpa=1002.73",                                    /*  539.120 мс */
  "ST,L3GD20,OK,addr=0x6B,who=0xD4",                                                /*  543.846 мс */
  "ST,LSM303D,WARN,addr=0x1D,who=0x49,role=FR06_accel,raw=7FF8,7FF8,33CD,"
  "a_g=2.938,ax_g=1.998,ay_g=1.998,az_g=0.808",                                     /*  547.989 мс */
  "ST,W25QXX,OK,jedec=EF4017,size_mbit=64",                                         /*  558.962 мс */
  "ST,OV7670,OK,pid=0x76,ver=0x73,vsync_hz=30",                                     /* 1564.532 мс */
  "ST,DHT11,OK,t_c=21.5,rh=34,raw=2209150545,crc=OK",                               /* 1794.514 мс */
  "ST,SERVO_PWM,OK,freq_hz=50,pulse_us=1500,note=no_feedback",                      /* 1799.345 мс */
  "ST,GPIO,OK,btn=1111,lock_b1=1,door=1,pir=0,mpu_int=0",                           /* 1805.061 мс */
  "ST,SUMMARY,WARN,ok=8,warn=1,fail=0,skip=1",                                      /* 1810.273 мс */
};
#define CAP_ST_UART_LINES (sizeof(cap_st_uart_lines) / sizeof(cap_st_uart_lines[0]))
#define CAP_ST_UART_IDX_BMP180  4U
#define CAP_ST_UART_IDX_DHT11   9U

/* ---- ЛР2, spi_w25q64_jedec.sr: команда j ---------------------------------------------
 * sigrok, fs = 12 МГц, 1 000 000 відліків; SCK (D2), MOSI (D3), MISO (D4), CS# (D5), TX (D6).
 *   sigrok-cli -i docs/captures/spi_w25q64_jedec.sr -P spi:clk=SCK:mosi=MOSI:miso=MISO:cs=CS -A spi
 * Один кадр CS#, 8.335 мс (відлік 100023): MOSI 9F FF FF FF, MISO FF EF 40 17.
 * UART 8.414 мс (відлік 100963): SPI,jedec=EF4017,err=ok */
static const uint8_t cap_w25q64_jedec_mosi[] = { 0x9F, 0xFF, 0xFF, 0xFF };
static const uint8_t cap_w25q64_jedec_miso[] = { 0xFF, 0xEF, 0x40, 0x17 };

#endif /* CAPTURE_VECTORS_H */
