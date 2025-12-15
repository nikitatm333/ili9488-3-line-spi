/*
 * ili9488_3line_hw9bit_draw.c
 *
 * ILI9488 driver for 3-line SPI (IM = 101)
 * Uses HARDWARE 9-bit SPI (D/C = bit8)
 *
 * Extended with simple text-draw sysfs interface:
 *  - pixel, hline, vline, rect (fill/outline), fill
 *
 * Based on user's original test driver.
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
#include <linux/string.h>
#include <linux/errno.h>
#include <linux/uaccess.h>

#define DRIVER_NAME "ili9488_3line_hw9bit_draw"

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
	u16                width;
	u16                height;
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

/* ---- basic commands ---- */

static int ili9488_set_window(struct ili9488 *lcd, u16 x0, u16 y0, u16 x1, u16 y1)
{
	u16 seq[12];
	int idx = 0;
	/* CASE: send 2A (column), 2B (page), then 2C (memory write) */
	seq[idx++] = W_CMD(0x2A);
	seq[idx++] = W_DATA((x0 >> 8) & 0xFF);
	seq[idx++] = W_DATA(x0 & 0xFF);
	seq[idx++] = W_DATA((x1 >> 8) & 0xFF);
	seq[idx++] = W_DATA(x1 & 0xFF);

	seq[idx++] = W_CMD(0x2B);
	seq[idx++] = W_DATA((y0 >> 8) & 0xFF);
	seq[idx++] = W_DATA(y0 & 0xFF);
	seq[idx++] = W_DATA((y1 >> 8) & 0xFF);
	seq[idx++] = W_DATA(y1 & 0xFF);

	seq[idx++] = W_CMD(0x2C);

	return spi_send_words(lcd->spi, seq, idx);
}

/* send 'count' pixels with same 3-bit color */
static int ili9488_write_pixels_same(struct ili9488 *lcd, int count, u8 color)
{
	const int chunk = 4096;
	u16 *buf;
	int sent = 0;
	int ret = 0;

	buf = kmalloc_array(min(count, chunk), sizeof(u16), GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	while (sent < count) {
		int n = min(chunk, count - sent);
		int i;
		for (i = 0; i < n; i++)
			buf[i] = W_DATA(color);
		ret = spi_send_words(lcd->spi, buf, n);
		if (ret)
			break;
		sent += n;
	}

	kfree(buf);
	return ret;
}

/* ---- high-level drawing primitives ---- */

static int ili9488_fill_color(struct ili9488 *lcd, u8 color)
{
	int total = lcd->width * lcd->height;
	int ret;

	ret = ili9488_set_window(lcd, 0, 0, lcd->width - 1, lcd->height - 1);
	if (ret)
		return ret;

	return ili9488_write_pixels_same(lcd, total, color);
}

static int ili9488_draw_pixel(struct ili9488 *lcd, u16 x, u16 y, u8 color)
{
	int ret;

	if (x >= lcd->width || y >= lcd->height)
		return -EINVAL;

	ret = ili9488_set_window(lcd, x, y, x, y);
	if (ret)
		return ret;

	return ili9488_write_pixels_same(lcd, 1, color);
}

static int ili9488_draw_hline(struct ili9488 *lcd, u16 x, u16 y, u16 len, u8 color)
{
	int ret;

	if (y >= lcd->height || x >= lcd->width)
		return -EINVAL;

	if (x + len > lcd->width)
		len = lcd->width - x;

	ret = ili9488_set_window(lcd, x, y, x + len - 1, y);
	if (ret)
		return ret;

	return ili9488_write_pixels_same(lcd, len, color);
}

static int ili9488_draw_vline(struct ili9488 *lcd, u16 x, u16 y, u16 len, u8 color)
{
	u16 i;
	int ret;

	if (x >= lcd->width || y >= lcd->height)
		return -EINVAL;

	if (y + len > lcd->height)
		len = lcd->height - y;

	/* draw as individual pixels (could be optimized) */
	for (i = 0; i < len; i++) {
		ret = ili9488_draw_pixel(lcd, x, y + i, color);
		if (ret)
			return ret;
	}
	return 0;
}

static int ili9488_draw_rect(struct ili9488 *lcd, u16 x, u16 y, u16 w, u16 h, u8 color, bool fill)
{
	int ret;

	if (w == 0 || h == 0)
		return -EINVAL;

	if (x >= lcd->width || y >= lcd->height)
		return -EINVAL;

	if (x + w > lcd->width)
		w = lcd->width - x;
	if (y + h > lcd->height)
		h = lcd->height - y;

	if (fill) {
		ret = ili9488_set_window(lcd, x, y, x + w - 1, y + h - 1);
		if (ret)
			return ret;
		return ili9488_write_pixels_same(lcd, (int)w * h, color);
	} else {
		/* outline: four lines */
		ret = ili9488_draw_hline(lcd, x, y, w, color);
		if (ret) return ret;
		if (h > 1) {
			ret = ili9488_draw_hline(lcd, x, y + h - 1, w, color);
			if (ret) return ret;
		}
		if (h > 2) {
			/* vertical sides excluding corners already drawn */
			ret = ili9488_draw_vline(lcd, x, y + 1, h - 2, color);
			if (ret) return ret;
			ret = ili9488_draw_vline(lcd, x + w - 1, y + 1, h - 2, color);
			if (ret) return ret;
		}
		return 0;
	}
}

/* ---- sysfs parsing ---- */

static int parse_u16(const char *s, u16 *out)
{
	unsigned long v;
	int ret = kstrtoul(s, 0, &v);
	if (ret)
		return ret;
	if (v > 0xFFFF)
		return -ERANGE;
	*out = (u16)v;
	return 0;
}

static int parse_u8(const char *s, u8 *out)
{
	unsigned long v;
	int ret = kstrtoul(s, 0, &v);
	if (ret)
		return ret;
	if (v > 0xFF)
		return -ERANGE;
	*out = (u8)v;
	return 0;
}

/* draw sysfs: write commands here */
static ssize_t draw_store(struct device *dev,
			 struct device_attribute *attr,
			 const char *buf, size_t count)
{
	struct spi_device *spi = to_spi_device(dev);
	struct ili9488 *lcd = spi_get_drvdata(spi);
	char *kbuf, *s, *token;
	int ret = 0;

	if (!lcd)
		return -ENODEV;

	/* copy user buf to NUL-terminated kernel buffer */
	kbuf = kstrndup(buf, count, GFP_KERNEL);
	if (!kbuf)
		return -ENOMEM;
	/* ensure termination */
	kbuf[count] = '\0';
	s = kbuf;

	mutex_lock(&lcd->lock);

	/* tokenize by whitespace */
	token = strsep(&s, " \t\n");
	if (!token || !*token) {
		ret = -EINVAL;
		goto out;
	}

	if (strcmp(token, "fill") == 0) {
		char *col_s = strsep(&s, " \t\n");
		u8 col;
		if (!col_s) { ret = -EINVAL; goto out; }
		if (parse_u8(col_s, &col) || col > 7) { ret = -EINVAL; goto out; }
		ret = ili9488_fill_color(lcd, col);
	} else if (strcmp(token, "pixel") == 0) {
		char *xs = strsep(&s, " \t\n");
		char *ys = strsep(&s, " \t\n");
		char *cs = strsep(&s, " \t\n");
		u16 x,y; u8 c;
		if (!xs || !ys || !cs) { ret = -EINVAL; goto out; }
		if (parse_u16(xs, &x) || parse_u16(ys, &y) || parse_u8(cs, &c) || c > 7) { ret = -EINVAL; goto out; }
		ret = ili9488_draw_pixel(lcd, x, y, c);
	} else if (strcmp(token, "hline") == 0) {
		char *xs = strsep(&s, " \t\n");
		char *ys = strsep(&s, " \t\n");
		char *ls = strsep(&s, " \t\n");
		char *cs = strsep(&s, " \t\n");
		u16 x,y,len; u8 c;
		if (!xs || !ys || !ls || !cs) { ret = -EINVAL; goto out; }
		if (parse_u16(xs, &x) || parse_u16(ys, &y) || parse_u16(ls, &len) || parse_u8(cs, &c) || c > 7) { ret = -EINVAL; goto out; }
		ret = ili9488_draw_hline(lcd, x, y, len, c);
	} else if (strcmp(token, "vline") == 0) {
		char *xs = strsep(&s, " \t\n");
		char *ys = strsep(&s, " \t\n");
		char *ls = strsep(&s, " \t\n");
		char *cs = strsep(&s, " \t\n");
		u16 x,y,len; u8 c;
		if (!xs || !ys || !ls || !cs) { ret = -EINVAL; goto out; }
		if (parse_u16(xs, &x) || parse_u16(ys, &y) || parse_u16(ls, &len) || parse_u8(cs, &c) || c > 7) { ret = -EINVAL; goto out; }
		ret = ili9488_draw_vline(lcd, x, y, len, c);
	} else if (strcmp(token, "rect") == 0) {
		char *xs = strsep(&s, " \t\n");
		char *ys = strsep(&s, " \t\n");
		char *ws = strsep(&s, " \t\n");
		char *hs = strsep(&s, " \t\n");
		char *cs = strsep(&s, " \t\n");
		char *style = strsep(&s, " \t\n");
		u16 x,y,w,h; u8 c;
		bool fill = false;
		if (!xs || !ys || !ws || !hs || !cs || !style) { ret = -EINVAL; goto out; }
		if (parse_u16(xs, &x) || parse_u16(ys, &y) || parse_u16(ws, &w) || parse_u16(hs, &h) || parse_u8(cs, &c) || c > 7) { ret = -EINVAL; goto out; }
		if (strcmp(style, "fill") == 0)
			fill = true;
		else if (strcmp(style, "outline") == 0)
			fill = false;
		else { ret = -EINVAL; goto out; }
		ret = ili9488_draw_rect(lcd, x, y, w, h, c, fill);
	} else {
		ret = -EINVAL;
	}

out:
	mutex_unlock(&lcd->lock);
	kfree(kbuf);
	if (ret)
		return ret;
	return count;
}

/* forward declarations for sysfs store callbacks */
static ssize_t color_store(struct device *dev,
                           struct device_attribute *attr,
                           const char *buf, size_t count);
static ssize_t draw_store(struct device *dev,
                          struct device_attribute *attr,
                          const char *buf, size_t count);


static DEVICE_ATTR_WO(color);
static DEVICE_ATTR_WO(draw);

/* ---- probe ---- */

static int ili9488_init(struct ili9488 *lcd);

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

	/* display resolution known from user's dts */
	lcd->width = 320;
	lcd->height = 480;

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

	/* create sysfs attributes */
	ret = device_create_file(&spi->dev, &dev_attr_color);
	if (ret)
		dev_warn(&spi->dev, "failed to create color attr: %d\n", ret);

	ret = device_create_file(&spi->dev, &dev_attr_draw);
	if (ret) {
		dev_warn(&spi->dev, "failed to create draw attr: %d\n", ret);
		/* not fatal */
	}

	return 0;
}

static int ili9488_remove(struct spi_device *spi)
{
	device_remove_file(&spi->dev, &dev_attr_draw);
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

/* ---- original init sequence ---- */

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

/* ---- color sysfs (existing) ---- */

static ssize_t color_store(struct device *dev,
			   struct device_attribute *attr,
			   const char *buf, size_t count)
{
	struct spi_device *spi = to_spi_device(dev);
	struct ili9488 *lcd = spi_get_drvdata(spi);
	unsigned long val;
	int ret;

	if (kstrtoul(buf, 0, &val) || val > 7)
		return -EINVAL;

	mutex_lock(&lcd->lock);
	if (!ili9488_fill_color(lcd, val))
		lcd->current_color = val;
	mutex_unlock(&lcd->lock);

	return count;
}

/* ---- module init ---- */

module_spi_driver(ili9488_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("tnv + assistant");
MODULE_DESCRIPTION("ILI9488 3-line SPI driver using HARDWARE 9-bit mode with simple draw sysfs");
