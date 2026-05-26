/*
 * ili9488_fb.c - Framebuffer driver for ILI9488 (3-bit mode, 8 colors)
 *
 * Interface: 3-line SPI, IM[2:0]=101, hardware 9-bit (bits_per_word=9)
 * Colors:    8 (RGB 1-1-1), COLMOD=0x01
 * Pixel:     1 byte per pixel, values 0x00-0x07
 *
 * Color map:
 *   0x00 = BLACK    0x01 = BLUE     0x02 = GREEN   0x03 = CYAN
 *   0x04 = RED      0x05 = MAGENTA  0x06 = YELLOW  0x07 = WHITE
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fb.h>
#include <linux/spi/spi.h>
#include <linux/of.h>
#include <linux/gpio/consumer.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/vmalloc.h>

#define DRIVER_NAME  "ili9488_fb"

#define LCD_WIDTH    320
#define LCD_HEIGHT   480
#define LCD_BPP      8
#define LCD_BUFSIZE  (LCD_WIDTH * LCD_HEIGHT)   /* 153600 байт */

#define FLUSH_CHUNK  2048       /* пикселей за один spi_sync */
#define DEFIO_DELAY  (HZ / 50) /* ~20ms между flush (50 fps max) */

struct ili9488_par {
	struct spi_device *spi;
	struct fb_info    *info;
	u8                *vmem;
	struct gpio_desc  *reset_gpiod;
	struct gpio_desc  *bl_gpiod;
};

/* ------------------------------------------------------------------ */
/* SPI 9-bit helpers                                                   */
/*                                                                      */
/* 9-bit word: бит8 = D/C, биты7..0 = данные                         */
/* D/C=0 → команда, D/C=1 → данные                                   */
/* ------------------------------------------------------------------ */

static int spi_9bit(struct spi_device *spi, u16 word)
{
	struct spi_transfer t;
	struct spi_message  m;

	memset(&t, 0, sizeof(t));
	t.tx_buf        = &word;
	t.len           = 2;
	t.bits_per_word = 9;

	spi_message_init(&m);
	spi_message_add_tail(&t, &m);
	return spi_sync(spi, &m);
}

static inline int lcd_cmd(struct spi_device *spi, u8 cmd)
{
	return spi_9bit(spi, (u16)cmd);
}

static inline int lcd_data(struct spi_device *spi, u8 data)
{
	return spi_9bit(spi, 0x100 | (u16)data);
}

/* ------------------------------------------------------------------ */
/* Hardware reset                                                       */
/* ------------------------------------------------------------------ */

static void ili9488_hw_reset(struct ili9488_par *par)
{
	if (!par->reset_gpiod)
		return;

	gpiod_set_value_cansleep(par->reset_gpiod, 0);
	msleep(20);
	gpiod_set_value_cansleep(par->reset_gpiod, 1);
	msleep(120);
}

/* ------------------------------------------------------------------ */
/* Display initialisation                                               */
/* ------------------------------------------------------------------ */

static void ili9488_init_display(struct ili9488_par *par)
{
	struct spi_device *spi = par->spi;

	ili9488_hw_reset(par);

	lcd_cmd(spi, 0x01); msleep(150); /* SWRESET   */
	lcd_cmd(spi, 0x11); msleep(120); /* SLEEP OUT */

	lcd_cmd(spi,  0x3A);             /* COLMOD: 3-bit/pixel */
	lcd_data(spi, 0x01);
	msleep(10);

	lcd_cmd(spi,  0x36);             /* MADCTL: MX=1, BGR=1 */
	lcd_data(spi, 0x48);
	msleep(10);

	lcd_cmd(spi, 0x21); msleep(10);  /* INVON  */
	lcd_cmd(spi, 0x13); msleep(10);  /* NORON  */
	lcd_cmd(spi, 0x29); msleep(50);  /* DISPON */

	dev_info(&par->spi->dev, "display init done\n");
}

/* ------------------------------------------------------------------ */
/* Set GRAM write window + RAMWR                                        */
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
/* Flush: отправка vmem на дисплей                                     */
/*                                                                      */
/* Каждый байт vmem (0x00-0x07) → 9-bit SPI слово: 0x100 | byte      */
/* FLUSH_CHUNK пикселей за один spi_sync (избегаем таймаут PL022)     */
/* ------------------------------------------------------------------ */

static void ili9488_flush(struct ili9488_par *par)
{
	struct spi_device *spi  = par->spi;
	u8                *vmem = par->vmem;
	u16               *chunk;
	int                sent = 0;
	int                total = LCD_WIDTH * LCD_HEIGHT;
	int                ret;

	chunk = kmalloc(FLUSH_CHUNK * sizeof(u16), GFP_KERNEL);
	if (!chunk)
		return;

	ili9488_set_window(spi, 0, 0, LCD_WIDTH - 1, LCD_HEIGHT - 1);

	while (sent < total) {
		struct spi_transfer t;
		struct spi_message  m;
		int n = min(FLUSH_CHUNK, total - sent);
		int i;

		for (i = 0; i < n; i++)
			chunk[i] = 0x100 | vmem[sent + i];

		memset(&t, 0, sizeof(t));
		t.tx_buf        = chunk;
		t.len           = n * sizeof(u16);
		t.bits_per_word = 9;

		spi_message_init(&m);
		spi_message_add_tail(&t, &m);

		ret = spi_sync(spi, &m);
		if (ret) {
			dev_err(&spi->dev, "flush: spi error %d at px %d\n",
				ret, sent);
			break;
		}

		sent += n;
	}

	kfree(chunk);
}

/* ------------------------------------------------------------------ */
/* Deferred IO                                                          */
/*                                                                      */
/* Вызывается ~20ms после записи в /dev/fb0 через mmap/write.         */
/* ------------------------------------------------------------------ */

static void ili9488_deferred_io(struct fb_info *info,
				struct list_head *pagelist)
{
	ili9488_flush(info->par);
}

static struct fb_deferred_io ili9488_defio = {
	.delay       = DEFIO_DELAY,
	.deferred_io = ili9488_deferred_io,
};

/* ------------------------------------------------------------------ */
/* fb_ops                                                               */
/* ------------------------------------------------------------------ */

static void ili9488_fb_fillrect(struct fb_info *info,
				const struct fb_fillrect *rect)
{
	cfb_fillrect(info, rect);
}

static void ili9488_fb_copyarea(struct fb_info *info,
				const struct fb_copyarea *area)
{
	cfb_copyarea(info, area);
}

static void ili9488_fb_imageblit(struct fb_info *info,
				 const struct fb_image *image)
{
	cfb_imageblit(info, image);
}

static struct fb_ops ili9488_fbops = {
	.owner        = THIS_MODULE,
	.fb_fillrect  = ili9488_fb_fillrect,
	.fb_copyarea  = ili9488_fb_copyarea,
	.fb_imageblit = ili9488_fb_imageblit,
};

/* ------------------------------------------------------------------ */
/* Screen info                                                          */
/* ------------------------------------------------------------------ */

static struct fb_fix_screeninfo ili9488_fix = {
	.id          = "ili9488_fb",
	.type        = FB_TYPE_PACKED_PIXELS,
	.visual      = FB_VISUAL_TRUECOLOR,
	.accel       = FB_ACCEL_NONE,
	.line_length = LCD_WIDTH,   /* 1 байт на пиксель */
};

static struct fb_var_screeninfo ili9488_var = {
	.xres           = LCD_WIDTH,
	.yres           = LCD_HEIGHT,
	.xres_virtual   = LCD_WIDTH,
	.yres_virtual   = LCD_HEIGHT,
	.bits_per_pixel = LCD_BPP,
	.red    = { .offset = 0, .length = 8, .msb_right = 0 },
	.green  = { .offset = 0, .length = 8, .msb_right = 0 },
	.blue   = { .offset = 0, .length = 8, .msb_right = 0 },
	.transp = { .offset = 0, .length = 0, .msb_right = 0 },
	.activate = FB_ACTIVATE_NOW,
	.vmode    = FB_VMODE_NONINTERLACED,
};

/* ------------------------------------------------------------------ */
/* Probe                                                                */
/* ------------------------------------------------------------------ */

static int ili9488_probe(struct spi_device *spi)
{
	struct ili9488_par *par;
	struct fb_info     *info;
	int ret;

	dev_info(&spi->dev, "probe start\n");

	/* 1. Выделяем fb_info + приватные данные */
	info = framebuffer_alloc(sizeof(struct ili9488_par), &spi->dev);
	if (!info)
		return -ENOMEM;

	par      = info->par;
	par->spi  = spi;
	par->info = info;
	spi_set_drvdata(spi, par);

	/* 2. Буфер видеопамяти */
	par->vmem = vzalloc(LCD_BUFSIZE);
	if (!par->vmem) {
		ret = -ENOMEM;
		goto err_fb_alloc;
	}

	/* 3. GPIO */
	par->reset_gpiod = devm_gpiod_get_optional(&spi->dev,
						   "reset", GPIOD_OUT_LOW);
	if (IS_ERR(par->reset_gpiod)) {
		dev_err(&spi->dev, "reset GPIO error\n");
		ret = PTR_ERR(par->reset_gpiod);
		goto err_vmem;
	}

	par->bl_gpiod = devm_gpiod_get_optional(&spi->dev,
						"backlight", GPIOD_OUT_LOW);
	if (IS_ERR(par->bl_gpiod)) {
		dev_err(&spi->dev, "backlight GPIO error\n");
		ret = PTR_ERR(par->bl_gpiod);
		goto err_vmem;
	}

	/* 4. SPI */
	spi->mode          = SPI_MODE_3;
	spi->bits_per_word = 9;
	spi->max_speed_hz  = 15000000;
	ret = spi_setup(spi);
	if (ret) {
		dev_err(&spi->dev, "spi_setup failed: %d\n", ret);
		goto err_vmem;
	}

	/* 5. Заполняем fb_info */
	info->fbops          = &ili9488_fbops;
	info->fix            = ili9488_fix;
	info->var            = ili9488_var;
	info->flags          = FBINFO_DEFAULT | FBINFO_VIRTFB;
	info->screen_base    = (char __iomem *)par->vmem;
	info->screen_size    = LCD_BUFSIZE;
	info->fix.smem_start = (unsigned long)par->vmem;
	info->fix.smem_len   = LCD_BUFSIZE;

	/* 6. Deferred IO */
	info->fbdefio = &ili9488_defio;
	fb_deferred_io_init(info);

	/* 7. Инициализация дисплея */
	ili9488_init_display(par);

	/* 8. Подсветка */
	if (par->bl_gpiod) {
		gpiod_set_value_cansleep(par->bl_gpiod, 1);
		msleep(10);
	}

	/* 9. Чёрный экран при старте */
	memset(par->vmem, 0x00, LCD_BUFSIZE);
	ili9488_flush(par);

	/* 10. Регистрация → создаётся /dev/fb0 */
	ret = register_framebuffer(info);
	if (ret) {
		dev_err(&spi->dev, "register_framebuffer failed: %d\n", ret);
		goto err_defio;
	}

	dev_info(&spi->dev, "registered /dev/fb%d, %dx%d, 8bpp (3-bit)\n",
		 info->node, LCD_WIDTH, LCD_HEIGHT);

	return 0;

err_defio:
	fb_deferred_io_cleanup(info);
err_vmem:
	vfree(par->vmem);
err_fb_alloc:
	framebuffer_release(info);
	return ret;
}

/* ------------------------------------------------------------------ */
/* Remove                                                               */
/* ------------------------------------------------------------------ */

static int ili9488_remove(struct spi_device *spi)
{
	struct ili9488_par *par  = spi_get_drvdata(spi);
	struct fb_info     *info = par->info;

	if (par->bl_gpiod)
		gpiod_set_value_cansleep(par->bl_gpiod, 0);

	unregister_framebuffer(info);
	fb_deferred_io_cleanup(info);
	vfree(par->vmem);
	framebuffer_release(info);

	dev_info(&spi->dev, "removed\n");
	return 0;
}

/* ------------------------------------------------------------------ */

static const struct of_device_id ili9488_fb_of_match[] = {
	{ .compatible = "ilitek,ili9488" },
	{ },
};
MODULE_DEVICE_TABLE(of, ili9488_fb_of_match);

static struct spi_driver ili9488_fb_driver = {
	.driver = {
		.name           = DRIVER_NAME,
		.of_match_table = ili9488_fb_of_match,
	},
	.probe  = ili9488_probe,
	.remove = ili9488_remove,
};

module_spi_driver(ili9488_fb_driver);

MODULE_AUTHOR("tnv");
MODULE_DESCRIPTION("ILI9488 framebuffer driver, 3-bit mode, 8 colors");
MODULE_LICENSE("GPL");
