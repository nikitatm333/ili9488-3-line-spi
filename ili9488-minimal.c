/*
 * ili9488_3line_hw9bit.c
 *
 * ILI9488 driver for 3-line SPI (IM = 101)
 * Uses HARDWARE 9-bit SPI (D/C = bit8)
 *
 * Tested logic for PL022 (HiSilicon)
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/spi/spi.h>
#include <linux/delay.h>
#include <linux/of.h>
#include <linux/gpio/consumer.h>
#include <linux/mutex.h>
#include <linux/device.h>
#include <linux/slab.h>

#define DRIVER_NAME "ili9488_3line_hw9bit"

/* colors (3-bit) */
#define COLOR_BLACK   0x0
#define COLOR_BLUE    0x1
#define COLOR_GREEN   0x2
#define COLOR_CYAN    0x3
#define COLOR_RED     0x4
#define COLOR_MAGENTA 0x5
#define COLOR_YELLOW  0x6
#define COLOR_WHITE   0x7

struct ili9488 {
	struct spi_device *spi;
	struct gpio_desc  *reset;
	struct gpio_desc  *bl;
	struct mutex       lock;
	u8                 current_color;
};

/* ---- helpers ---- */

static inline u16 W_CMD(u8 cmd)
{
	return (0 << 8) | cmd;
}

static inline u16 W_DATA(u8 data)
{
	return (1 << 8) | data;
}

/* ---- SPI send helpers (HW 9-bit) ---- */

static int spi_send_words(struct spi_device *spi, const u16 *buf, int nwords)
{
	struct spi_transfer t = {
		.tx_buf = buf,
		.len = nwords * 2,
		.bits_per_word = 9,
	};
	struct spi_message m;

	spi_message_init(&m);
	spi_message_add_tail(&t, &m);
	return spi_sync(spi, &m);
}

/* ---- reset ---- */

static void ili9488_hw_reset(struct ili9488 *lcd)
{
	if (!lcd->reset)
		return;

	gpiod_set_value_cansleep(lcd->reset, 0);
	msleep(20);
	gpiod_set_value_cansleep(lcd->reset, 1);
	msleep(120);
}

/* ---- init sequence ---- */

static int ili9488_init(struct ili9488 *lcd)
{
	struct spi_device *spi = lcd->spi;
	u16 seq[8];
	int ret;

	ili9488_hw_reset(lcd);

	seq[0] = W_CMD(0x01); /* SWRESET */
	ret = spi_send_words(spi, seq, 1);
	if (ret) return ret;
	msleep(150);

	seq[0] = W_CMD(0x11); /* SLEEP OUT */
	ret = spi_send_words(spi, seq, 1);
	if (ret) return ret;
	msleep(120);

	seq[0] = W_CMD(0x3A); /* COLMOD */
	seq[1] = W_DATA(0x01); /* 3-bit */
	ret = spi_send_words(spi, seq, 2);
	if (ret) return ret;

	seq[0] = W_CMD(0x36); /* MADCTL */
	seq[1] = W_DATA(0x48);
	ret = spi_send_words(spi, seq, 2);
	if (ret) return ret;

	seq[0] = W_CMD(0x21); /* INVON */
	ret = spi_send_words(spi, seq, 1);
	if (ret) return ret;

	seq[0] = W_CMD(0x13); /* NORON */
	ret = spi_send_words(spi, seq, 1);
	if (ret) return ret;

	seq[0] = W_CMD(0x29); /* DISPON */
	ret = spi_send_words(spi, seq, 1);
	if (ret) return ret;

	return 0;
}

/* ---- fill screen ---- */

static int ili9488_fill_color(struct ili9488 *lcd, u8 color)
{
	struct spi_device *spi = lcd->spi;
	const int total = 320 * 480;
	const int chunk = 4096;
	u16 *buf;
	int sent = 0;
	int ret;

	/* set window */
	u16 win[] = {
		W_CMD(0x2A), W_DATA(0), W_DATA(0), W_DATA(1), W_DATA(0x3F),
		W_CMD(0x2B), W_DATA(0), W_DATA(0), W_DATA(1), W_DATA(0xDF),
		W_CMD(0x2C)
	};

	ret = spi_send_words(spi, win, ARRAY_SIZE(win));
	if (ret) return ret;

	buf = kmalloc_array(chunk, sizeof(u16), GFP_KERNEL);
	if (!buf) return -ENOMEM;

	while (sent < total) {
		int n = min(chunk, total - sent);
		int i;

		for (i = 0; i < n; i++)
			buf[i] = W_DATA(color);

		ret = spi_send_words(spi, buf, n);
		if (ret)
			break;

		sent += n;
	}

	kfree(buf);
	return ret;
}

/* ---- sysfs ---- */

static ssize_t color_store(struct device *dev,
			   struct device_attribute *attr,
			   const char *buf, size_t count)
{
	struct spi_device *spi = to_spi_device(dev);
	struct ili9488 *lcd = spi_get_drvdata(spi);
	unsigned long val;

	if (kstrtoul(buf, 0, &val) || val > 7)
		return -EINVAL;

	mutex_lock(&lcd->lock);
	if (!ili9488_fill_color(lcd, val))
		lcd->current_color = val;
	mutex_unlock(&lcd->lock);

	return count;
}

static DEVICE_ATTR_WO(color);

/* ---- probe ---- */

static int ili9488_probe(struct spi_device *spi)
{
	struct ili9488 *lcd;
	int ret;

	lcd = devm_kzalloc(&spi->dev, sizeof(*lcd), GFP_KERNEL);
	if (!lcd)
		return -ENOMEM;

	lcd->spi = spi;
	mutex_init(&lcd->lock);
	spi_set_drvdata(spi, lcd);

	lcd->reset = devm_gpiod_get_optional(&spi->dev, "reset", GPIOD_OUT_HIGH);
	lcd->bl    = devm_gpiod_get_optional(&spi->dev, "backlight", GPIOD_OUT_HIGH);

	/* ---- HARD REQUIREMENT ---- */
	spi->mode = SPI_MODE_3;
	spi->bits_per_word = 9;
	spi->max_speed_hz = 1000000;

	ret = spi_setup(spi);
	if (ret) {
		dev_err(&spi->dev,
			"SPI controller does NOT support 9-bit mode (%d)\n", ret);
		return -ENODEV;
	}

	dev_info(&spi->dev, "SPI 9-bit mode ENABLED\n");

	if (lcd->bl)
		gpiod_set_value_cansleep(lcd->bl, 1);

	ret = ili9488_init(lcd);
	if (ret)
		return ret;

	device_create_file(&spi->dev, &dev_attr_color);

	return 0;
}

static int ili9488_remove(struct spi_device *spi)
{
	device_remove_file(&spi->dev, &dev_attr_color);
	return 0;
}

static const struct of_device_id ili9488_of_match[] = {
	{ .compatible = "ilitek,ili9488" },
	{ }
};
MODULE_DEVICE_TABLE(of, ili9488_of_match);

static struct spi_driver ili9488_driver = {
	.driver = {
		.name = DRIVER_NAME,
		.of_match_table = ili9488_of_match,
	},
	.probe  = ili9488_probe,
	.remove = ili9488_remove,
};

module_spi_driver(ili9488_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("tnv");
MODULE_DESCRIPTION("ILI9488 3-line SPI driver using HARDWARE 9-bit mode");
