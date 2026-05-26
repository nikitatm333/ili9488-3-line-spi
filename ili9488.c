/*
 * ili9488_fb.c - Framebuffer driver for ILI9488 in 3-bit mode (8 colors)
 *
 * Работает в том же режиме, что и ili9488_3bit.c (COLMOD=0x01)
 * Использует hardware 9-bit SPI (bits_per_word=9)
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
#define LCD_BPP      8                    /* 1 байт = 1 пиксель (3 бита цвета) */
#define LCD_BUFSIZE  (LCD_WIDTH * LCD_HEIGHT)   /* 153600 байт */

/* Чанк для flush (как в рабочем 3bit драйвере) */
#define FLUSH_CHUNK  2048

/* Deferred IO ~50 fps */
#define DEFIO_DELAY  (HZ / 50)

struct ili9488_par {
	struct spi_device *spi;
	struct fb_info    *info;
	u8                *vmem;           /* framebuffer in RAM */
	struct gpio_desc  *reset_gpiod;
	struct gpio_desc  *bl_gpiod;
};

/* ------------------------------------------------------------------ */
/* SPI 9-bit helpers (точно как в твоём рабочем драйвере)           */
/* ------------------------------------------------------------------ */
static int spi_9bit(struct spi_device *spi, u16 word)
{
	struct spi_transfer t = {
		.tx_buf        = &word,
		.len           = 2,
		.bits_per_word = 9,
	};
	struct spi_message m;

	spi_message_init(&m);
	spi_message_add_tail(&t, &m);
	return spi_sync(spi, &m);
}

static inline int lcd_cmd(struct spi_device *spi, u8 cmd)
{
	return spi_9bit(spi, (u16)cmd);           /* D/C=0 */
}

static inline int lcd_data(struct spi_device *spi, u8 data)
{
	return spi_9bit(spi, 0x100 | (u16)data);  /* D/C=1 */
}

/* ------------------------------------------------------------------ */
/* Hardware reset                                                    */
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


static void ili9488_init_display(struct ili9488_par *par)
{
	struct spi_device *spi = par->spi;

	ili9488_hw_reset(par);

	lcd_cmd(spi, 0x01); msleep(150); /* SWRESET */
	lcd_cmd(spi, 0x11); msleep(120); /* SLEEP OUT */

	lcd_cmd(spi, 0x3A);              /* COLMOD: 3-bit/pixel */
	lcd_data(spi, 0x01);
	msleep(10);

	lcd_cmd(spi, 0x36);              /* MADCTL */
	lcd_data(spi, 0x48);
	msleep(10);

	lcd_cmd(spi, 0x21); msleep(10);  /* INVON */
	lcd_cmd(spi, 0x13); msleep(10);  /* NORON */
	lcd_cmd(spi, 0x29); msleep(50);  /* DISPON */

	pr_info("ILI9488: init_display done (3-bit mode)\n");
}

/* ------------------------------------------------------------------ */
/* Set window                                                        */
/* ------------------------------------------------------------------ */
static void ili9488_set_window(struct spi_device *spi,
			       u16 x0, u16 y0, u16 x1, u16 y1)
{
	lcd_cmd(spi, 0x2A);
	lcd_data(spi, x0 >> 8); lcd_data(spi, x0 & 0xFF);
	lcd_data(spi, x1 >> 8); lcd_data(spi, x1 & 0xFF);

	lcd_cmd(spi, 0x2B);
	lcd_data(spi, y0 >> 8); lcd_data(spi, y0 & 0xFF);
	lcd_data(spi, y1 >> 8); lcd_data(spi, y1 & 0xFF);

	lcd_cmd(spi, 0x2C); /* RAMWR */
}

/* ------------------------------------------------------------------ */
/* Flush — адаптировано под 8bpp (1 байт = 1 пиксель)               */
/* ------------------------------------------------------------------ */
static void ili9488_flush(struct ili9488_par *par)
{
	struct spi_device *spi = par->spi;
	u8                *vmem = par->vmem;
	u16               *chunk;
	int                sent = 0;
	int                total = LCD_WIDTH * LCD_HEIGHT;
	int                ret;

	chunk = kmalloc(FLUSH_CHUNK * sizeof(u16), GFP_KERNEL);
	if (!chunk)
		return;

	ili9488_set_window(spi, 0, 0, LCD_WIDTH-1, LCD_HEIGHT-1);

	while (sent < total) {
		int n = min(FLUSH_CHUNK, total - sent);
		int i;

		for (i = 0; i < n; i++)
			chunk[i] = 0x100 | vmem[sent + i];   /* D/C = 1 */

		struct spi_transfer t = {
			.tx_buf        = chunk,
			.len           = n * sizeof(u16),
			.bits_per_word = 9,
		};
		struct spi_message m;

		spi_message_init(&m);
		spi_message_add_tail(&t, &m);

		ret = spi_sync(spi, &m);
		if (ret) {
			dev_err(&spi->dev, "flush error %d at pixel %d\n", ret, sent);
			break;
		}
		sent += n;
	}

	kfree(chunk);
}

/* ------------------------------------------------------------------ */
/* Deferred IO                                                       */
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
/* fb_ops                                                            */
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
/* Screen info                                                       */
/* ------------------------------------------------------------------ */
static struct fb_fix_screeninfo ili9488_fix = {
	.id          = "ili9488_fb",
	.type        = FB_TYPE_PACKED_PIXELS,
	.visual      = FB_VISUAL_TRUECOLOR,     /* можно PSEUDOCOLOR, но TRUECOLOR проще */
	.accel       = FB_ACCEL_NONE,
	.line_length = LCD_WIDTH,               /* 1 байт на пиксель */
};

static struct fb_var_screeninfo ili9488_var = {
	.xres           = LCD_WIDTH,
	.yres           = LCD_HEIGHT,
	.xres_virtual   = LCD_WIDTH,
	.yres_virtual   = LCD_HEIGHT,
	.bits_per_pixel = LCD_BPP,
	.red            = { .offset = 0, .length = 8, .msb_right = 0 },
	.green          = { .offset = 0, .length = 8, .msb_right = 0 },
	.blue           = { .offset = 0, .length = 8, .msb_right = 0 },
	.transp         = { .offset = 0, .length = 0 },
	.activate       = FB_ACTIVATE_NOW,
	.vmode          = FB_VMODE_NONINTERLACED,
};

/* ------------------------------------------------------------------ */
/* Probe                                                             */
/* ------------------------------------------------------------------ */
static int ili9488_probe(struct spi_device *spi)
{
	struct ili9488_par *par;
	struct fb_info *info;
	int ret;

	dev_info(&spi->dev, "probe start\n");

	info = framebuffer_alloc(sizeof(struct ili9488_par), &spi->dev);
	if (!info)
		return -ENOMEM;

	par = info->par;
	par->spi = spi;
	par->info = info;
	spi_set_drvdata(spi, par);

	/* Allocate framebuffer memory */
	par->vmem = vzalloc(LCD_BUFSIZE);
	if (!par->vmem) {
		ret = -ENOMEM;
		goto err_fb_alloc;
	}

	/* GPIOs */
	par->reset_gpiod = devm_gpiod_get_optional(&spi->dev, "reset", GPIOD_OUT_LOW);
	par->bl_gpiod    = devm_gpiod_get_optional(&spi->dev, "backlight", GPIOD_OUT_LOW);

	/* SPI setup */
	spi->mode = SPI_MODE_3;
	spi->bits_per_word = 9;
	spi->max_speed_hz = 15000000;
	ret = spi_setup(spi);
	if (ret) {
		dev_err(&spi->dev, "spi_setup failed: %d\n", ret);
		goto err_vmem;
	}

	/* Fill fb_info */
	info->fbops = &ili9488_fbops;
	info->fix = ili9488_fix;
	info->var = ili9488_var;
	info->flags = FBINFO_DEFAULT | FBINFO_VIRTFB;
	info->screen_base = (char __iomem *)par->vmem;
	info->screen_size = LCD_BUFSIZE;
	info->fix.smem_start = (unsigned long)par->vmem;
	info->fix.smem_len = LCD_BUFSIZE;

	/* Deferred IO */
	info->fbdefio = &ili9488_defio;
	fb_deferred_io_init(info);

	/* Hardware init */
	ili9488_init_display(par);

	/* Backlight */
	if (par->bl_gpiod) {
		gpiod_set_value_cansleep(par->bl_gpiod, 1);
		msleep(10);
	}

	/* Register */
	ret = register_framebuffer(info);
	if (ret) {
		dev_err(&spi->dev, "register_framebuffer failed: %d\n", ret);
		goto err_defio;
	}

	dev_info(&spi->dev, "registered /dev/fb0, 320x480, 3-bit mode\n");

	/* Тестовые заливки как в твоём рабочем драйвере */
	memset(par->vmem, 0x04, LCD_BUFSIZE);  /* RED */
	ili9488_flush(par);
	msleep(800);

	memset(par->vmem, 0x02, LCD_BUFSIZE);  /* GREEN */
	ili9488_flush(par);
	msleep(800);

	memset(par->vmem, 0x01, LCD_BUFSIZE);  /* BLUE */
	ili9488_flush(par);
	msleep(800);

	memset(par->vmem, 0x07, LCD_BUFSIZE);  /* WHITE */
	ili9488_flush(par);

	pr_info("ILI9488: test colors done\n");

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
/* Remove                                                            */
/* ------------------------------------------------------------------ */
static int ili9488_remove(struct spi_device *spi)
{
	struct ili9488_par *par = spi_get_drvdata(spi);
	struct fb_info *info = par->info;

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
MODULE_DESCRIPTION("ILI9488 framebuffer driver (3-bit mode, 8 colors)");
MODULE_LICENSE("GPL");
