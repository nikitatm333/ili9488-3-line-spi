/*
 * ili9488_3bit.c - Драйвер для ILI9488 в 3-битном SPI режиме
 *
 * КЛЮЧЕВОЕ: используем hardware 9-bit SPI (bits_per_word = 9).
 *
 * Проблема предыдущих версий: при 8-bit SPI отправлялось 2 байта (16 бит)
 * на одно 9-битное слово. Лишние 7 бит накапливались между CS-пульсами
 * и сдвигали D/C-бит, из-за чего часть пикселей трактовалась как команды.
 * Результат — заливка только 3/4 экрана.
 *
 * С bits_per_word = 9: каждый u16 в буфере = ровно 9 бит на проводе.
 * Никаких лишних бит, никакого накопления.
 *
 * Формат 9-битного слова (u16, MSB first):
 *   бит 8 (bit15 не используется, значащий MSB = бит8) = D/C
 *   биты 7..0 = данные
 *   → u16 = (dc << 8) | data
 *
 * Заливка: чанки по 2048 пикселей (18432 бит @ 5МГц = ~3.7 мс),
 * хорошо укладывается в таймаут PL022 polling.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/spi/spi.h>
#include <linux/delay.h>
#include <linux/of.h>
#include <linux/gpio/consumer.h>
#include <linux/slab.h>

#define DRIVER_NAME "ili9488_3bit"

#define LCD_WIDTH   320
#define LCD_HEIGHT  480

/* Пикселей за один spi_write при заливке */
#define FILL_CHUNK  2048

struct ili9488_dev {
	struct spi_device *spi;
	struct gpio_desc  *reset_gpiod;
	struct gpio_desc  *bl_gpiod;
};

/* ------------------------------------------------------------------ */
/* Отправка одного 9-битного слова                                     */
/* u16 value: бит8 = D/C, биты7..0 = данные                           */
/* ------------------------------------------------------------------ */

static int spi_9bit_word(struct spi_device *spi, u16 word)
{
	struct spi_transfer t = {
		.tx_buf        = &word,
		.len           = 2,         /* 2 байта в памяти... */
		.bits_per_word = 9,         /* ...но на проводе ровно 9 бит */
	};
	struct spi_message m;

	spi_message_init(&m);
	spi_message_add_tail(&t, &m);
	return spi_sync(spi, &m);
}

static inline int lcd_cmd(struct spi_device *spi, u8 cmd)
{
	/* D/C=0 → бит 8 = 0 */
	return spi_9bit_word(spi, (u16)cmd);
}

static inline int lcd_data(struct spi_device *spi, u8 data)
{
	/* D/C=1 → бит 8 = 1 */
	return spi_9bit_word(spi, 0x100 | data);
}

/* ------------------------------------------------------------------ */
/* Аппаратный сброс                                                    */
/* ------------------------------------------------------------------ */

static void ili9488_hw_reset(struct ili9488_dev *lcd)
{
	if (!lcd->reset_gpiod)
		return;
	gpiod_set_value_cansleep(lcd->reset_gpiod, 0);
	msleep(20);
	gpiod_set_value_cansleep(lcd->reset_gpiod, 1);
	msleep(120);
	pr_info("ILI9488: hw reset done\n");
}

/* ------------------------------------------------------------------ */
/* Инициализация                                                        */
/* ------------------------------------------------------------------ */

static void ili9488_init(struct ili9488_dev *lcd)
{
    struct spi_device *spi = lcd->spi;

    ili9488_hw_reset(lcd);

    lcd_cmd(spi, 0x01); msleep(150); /* SWRESET   */
    lcd_cmd(spi, 0x11); msleep(120); /* SLEEP OUT */

    lcd_cmd(spi, 0x3A);              /* COLMOD: 3-bit/pixel */
    lcd_data(spi, 0x01);
    msleep(10);

    lcd_cmd(spi, 0x36);              /* MADCTL: MX=1, BGR=1 — как в рабочем коде */
    lcd_data(spi, 0x48);
    msleep(10);

    lcd_cmd(spi, 0x21); msleep(10);  /* INVON — нужен для IPS Normally Black */

    lcd_cmd(spi, 0x13); msleep(10);  /* NORON  */
    lcd_cmd(spi, 0x29); msleep(50);  /* DISPON */

    pr_info("ILI9488: init done\n");
}

/* ------------------------------------------------------------------ */
/* Установка окна + RAMWR                                              */
/* ------------------------------------------------------------------ */

static void ili9488_set_window(struct spi_device *spi,
			       u16 x0, u16 y0, u16 x1, u16 y1)
{
	lcd_cmd(spi,  0x2A);
	lcd_data(spi, x0 >> 8); lcd_data(spi, x0 & 0xFF);
	lcd_data(spi, x1 >> 8); lcd_data(spi, x1 & 0xFF);

	lcd_cmd(spi,  0x2B);
	lcd_data(spi, y0 >> 8); lcd_data(spi, y0 & 0xFF);
	lcd_data(spi, y1 >> 8); lcd_data(spi, y1 & 0xFF);

	lcd_cmd(spi,  0x2C); /* RAMWR */
}

/* ------------------------------------------------------------------ */
/* Цвета (RGB 1-1-1)                                                   */
/* ------------------------------------------------------------------ */

#define COLOR_BLACK   0x00
#define COLOR_BLUE    0x01
#define COLOR_GREEN   0x02
#define COLOR_CYAN    0x03
#define COLOR_RED     0x04
#define COLOR_MAGENTA 0x05
#define COLOR_YELLOW  0x06
#define COLOR_WHITE   0x07

/* ------------------------------------------------------------------ */
/* Заливка экрана                                                       */
/*                                                                      */
/* Буфер u16[FILL_CHUNK]: каждый элемент = 0x100|color (D/C=1).       */
/* spi_write с bits_per_word=9 отправляет ровно 9 бит на слово.       */
/* Чанк 2048 пикс × 9 бит @ 5МГц = ~3.7 мс → нет таймаута PL022.    */
/* ------------------------------------------------------------------ */

static void ili9488_fill(struct spi_device *spi, u8 color)
{
	const int total = LCD_WIDTH * LCD_HEIGHT; /* 153600 */
	u16 pixel = 0x100 | (color & 0x07);
	u16 *buf;
	struct spi_transfer t;
	struct spi_message m;
	int i, sent = 0, ret;

	buf = kmalloc(FILL_CHUNK * sizeof(u16), GFP_KERNEL);
	if (!buf) {
		pr_err("ILI9488: fill: kmalloc failed\n");
		return;
	}

	for (i = 0; i < FILL_CHUNK; i++)
		buf[i] = pixel;

	pr_info("ILI9488: fill color=0x%02X\n", color);

	ili9488_set_window(spi, 0, 0, LCD_WIDTH - 1, LCD_HEIGHT - 1);

	while (sent < total) {
		int n = min(FILL_CHUNK, total - sent);

		memset(&t, 0, sizeof(t));
		t.tx_buf        = buf;
		t.len           = n * sizeof(u16);
		t.bits_per_word = 9;

		spi_message_init(&m);
		spi_message_add_tail(&t, &m);

		ret = spi_sync(spi, &m);
		if (ret) {
			pr_err("ILI9488: fill: spi err=%d at px %d\n",
			       ret, sent);
			break;
		}
		sent += n;
	}

	kfree(buf);
	pr_info("ILI9488: fill done (%d px)\n", sent);
}

/* ------------------------------------------------------------------ */
/* Probe / Remove                                                       */
/* ------------------------------------------------------------------ */

static int ili9488_3bit_probe(struct spi_device *spi)
{
	struct device      *dev = &spi->dev;
	struct ili9488_dev *lcd;
	int ret;

	pr_info("ILI9488: probe start\n");

	lcd = devm_kzalloc(dev, sizeof(*lcd), GFP_KERNEL);
	if (!lcd)
		return -ENOMEM;

	lcd->spi = spi;
	spi_set_drvdata(spi, lcd);

	lcd->reset_gpiod = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(lcd->reset_gpiod)) {
		dev_err(dev, "reset GPIO error\n");
		return PTR_ERR(lcd->reset_gpiod);
	}

	lcd->bl_gpiod = devm_gpiod_get_optional(dev, "backlight", GPIOD_OUT_LOW);
	if (IS_ERR(lcd->bl_gpiod)) {
		dev_err(dev, "backlight GPIO error\n");
		return PTR_ERR(lcd->bl_gpiod);
	}

	/* Hardware 9-bit SPI — контроллер сам отправляет ровно 9 бит */
	spi->mode          = SPI_MODE_3;
	spi->bits_per_word = 9;
	spi->max_speed_hz  = 5000000;

	ret = spi_setup(spi);
	if (ret) {
		dev_err(dev, "spi_setup 9-bit failed: %d\n", ret);
		/*
		 * Некоторые SPI-контроллеры не поддерживают 9-bit.
		 * Если spi_setup вернул ошибку — плата не поддерживает.
		 */
		return ret;
	}

	pr_info("ILI9488: SPI mode=%u speed=%u bpw=%u\n",
		spi->mode, spi->max_speed_hz, spi->bits_per_word);

	if (lcd->bl_gpiod) {
		gpiod_set_value_cansleep(lcd->bl_gpiod, 1);
		msleep(10);
		pr_info("ILI9488: backlight ON\n");
	}

	ili9488_init(lcd);

	pr_info("ILI9488: RED\n");
	ili9488_fill(spi, COLOR_RED);
	msleep(2000);

	pr_info("ILI9488: GREEN\n");
	ili9488_fill(spi, COLOR_GREEN);
	msleep(2000);

	pr_info("ILI9488: BLUE\n");
	ili9488_fill(spi, COLOR_BLUE);
	msleep(2000);

	pr_info("ILI9488: WHITE\n");
	ili9488_fill(spi, COLOR_WHITE);
	msleep(2000);

	pr_info("ILI9488: BLACK\n");
	ili9488_fill(spi, COLOR_BLACK);

	pr_info("ILI9488: probe done\n");
	return 0;
}

static int ili9488_3bit_remove(struct spi_device *spi)
{
	struct ili9488_dev *lcd = spi_get_drvdata(spi);

	if (lcd && lcd->bl_gpiod)
		gpiod_set_value_cansleep(lcd->bl_gpiod, 0);

	pr_info("ILI9488: removed\n");
	return 0;
}

static const struct of_device_id ili9488_3bit_of_match[] = {
	{ .compatible = "ilitek,ili9488" },
	{ },
};
MODULE_DEVICE_TABLE(of, ili9488_3bit_of_match);

static struct spi_driver ili9488_3bit_driver = {
	.driver = {
		.name           = DRIVER_NAME,
		.of_match_table = ili9488_3bit_of_match,
	},
	.probe  = ili9488_3bit_probe,
	.remove = ili9488_3bit_remove,
};

module_spi_driver(ili9488_3bit_driver);

MODULE_AUTHOR("tnv");
MODULE_DESCRIPTION("ILI9488 3-bit SPI, hardware 9-bit mode");
MODULE_LICENSE("GPL");
