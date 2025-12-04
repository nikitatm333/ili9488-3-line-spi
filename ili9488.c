/*
 * ili9488_3line.c - ILI9488 3-line (9-bit) SPI driver
 * IM[2:0] = 101 (DBI Type C Option 1 - 3-line SPI)
 * 9-bit SPI words: bit8 = D/C, bits7-0 = payload
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/spi/spi.h>
#include <linux/delay.h>
#include <linux/of.h>
#include <linux/gpio/consumer.h>
#include <linux/lcd.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/ctype.h>
#include <linux/fb.h>

#define DRIVER_NAME "ili9488"

/* Commands */
#define CMD_NOP             0x00
#define CMD_SWRESET         0x01
#define CMD_SLEEP_IN        0x10
#define CMD_SLEEP_OUT       0x11
#define CMD_PARTIAL_MODE_ON 0x12
#define CMD_NORMAL_MODE_ON  0x13
#define CMD_DISPLAY_OFF     0x28
#define CMD_DISPLAY_ON      0x29
#define CMD_CASET           0x2A
#define CMD_PASET           0x2B
#define CMD_RAMWR           0x2C
#define CMD_COLMOD          0x3A
#define CMD_MADCTL          0x36

struct ili9488_dev {
    struct spi_device *spi;
    struct gpio_desc *reset_gpiod;
    struct gpio_desc *bl_gpiod;
    struct lcd_device *ld;
    u32 width;
    u32 height;
    int power;
};

/* Pack 9-bit word: bit8 = D/C (0=cmd, 1=data), bits7-0 = data */
static inline u16 ili9488_pack9(u8 data, int is_data)
{
    return cpu_to_le16(((is_data ? 1 : 0) << 8) | (data & 0xFF));
}

/* Send command (D/C = 0) */
static int ili9488_cmd(struct spi_device *spi, u8 cmd)
{
    u16 word = ili9488_pack9(cmd, 0);
    int ret;
    
    ret = spi_write(spi, &word, 2); /* 2 bytes for 9-bit word */
    if (ret)
        dev_err(&spi->dev, "CMD 0x%02x failed: %d\n", cmd, ret);
    return ret;
}

/* Send data (D/C = 1) */
static int ili9488_data(struct spi_device *spi, u8 data)
{
    u16 word = ili9488_pack9(data, 1);
    return spi_write(spi, &word, 2);
}

/* Send multiple data bytes */
static int ili9488_data_bulk(struct spi_device *spi, const u8 *buf, size_t len)
{
    u16 *words;
    size_t i;
    int ret;
    
    if (!len)
        return 0;
    
    words = kmalloc(len * 2, GFP_KERNEL); /* 2 bytes per 9-bit word */
    if (!words)
        return -ENOMEM;
    
    for (i = 0; i < len; i++)
        words[i] = ili9488_pack9(buf[i], 1);
    
    ret = spi_write(spi, words, len * 2);
    kfree(words);
    return ret;
}

/* Write command with parameters */
static int ili9488_write_reg(struct spi_device *spi, u8 cmd, const u8 *params, size_t plen)
{
    int ret;
    
    ret = ili9488_cmd(spi, cmd);
    if (ret)
        return ret;
    
    if (plen)
        ret = ili9488_data_bulk(spi, params, plen);
    
    return ret;
}

/* Hardware reset */
static void ili9488_hw_reset(struct ili9488_dev *lcd)
{
    if (!lcd->reset_gpiod)
        return;
    
    gpiod_set_value_cansleep(lcd->reset_gpiod, 0);
    msleep(10);
    gpiod_set_value_cansleep(lcd->reset_gpiod, 1);
    msleep(120); /* Wait after reset */
}

/* Simple initialization sequence for 3-line SPI */
static int ili9488_init_display(struct ili9488_dev *lcd)
{
    struct spi_device *spi = lcd->spi;
    u8 params[16];
    int ret;
    
    dev_info(&spi->dev, "Initializing ILI9488 (3-line SPI)\n");
    
    /* 1. Hardware reset */
    ili9488_hw_reset(lcd);
    
    /* 2. Software Reset */
    ret = ili9488_cmd(spi, CMD_SWRESET);
    msleep(120); /* Important: wait 120ms after reset */
    
    /* 3. Sleep Out */
    ret = ili9488_cmd(spi, CMD_SLEEP_OUT);
    msleep(5);
    
    /* 4. Interface Pixel Format: RGB565 = 0x55, RGB666 = 0x66 */
    params[0] = 0x55; /* 16-bit RGB565 */
    ret = ili9488_write_reg(spi, CMD_COLMOD, params, 1);
    msleep(10);
    
    /* 5. Memory Access Control */
    params[0] = 0x48; /* MY=0, MX=0, MV=0, ML=0, BGR=1, MH=0 */
    ret = ili9488_write_reg(spi, CMD_MADCTL, params, 1);
    msleep(10);
    
    /* 6. Display ON */
    ret = ili9488_cmd(spi, CMD_DISPLAY_ON);
    msleep(50);
    
    /* 7. Normal Display Mode ON */
    ret = ili9488_cmd(spi, CMD_NORMAL_MODE_ON);
    msleep(10);
    
    dev_info(&spi->dev, "Display initialized\n");
    return 0;
}

/* Fill screen with RGB565 color - OPTIMIZED VERSION */
static int ili9488_fill_screen(struct ili9488_dev *lcd, u16 color)
{
    struct spi_device *spi = lcd->spi;
    u8 params[4];
    u16 *line_buffer;
    u32 line_size = lcd->width * 2; /* 2 bytes per pixel */
    u32 total_pixels = lcd->width * lcd->height;
    u32 i;
    int ret;
    
    dev_info(&spi->dev, "Filling screen: 0x%04X\n", color);
    
    /* Set column address (0-319) */
    params[0] = 0x00; /* Start high byte */
    params[1] = 0x00; /* Start low byte */
    params[2] = 0x01; /* End high byte (319 = 0x013F) */
    params[3] = 0x3F; /* End low byte */
    ret = ili9488_write_reg(spi, CMD_CASET, params, 4);
    if (ret) return ret;
    
    /* Set page address (0-479) */
    params[0] = 0x00; /* Start high byte */
    params[1] = 0x00; /* Start low byte */
    params[2] = 0x01; /* End high byte (479 = 0x01DF) */
    params[3] = 0xDF; /* End low byte */
    ret = ili9488_write_reg(spi, CMD_PASET, params, 4);
    if (ret) return ret;
    
    /* Start memory write */
    ret = ili9488_cmd(spi, CMD_RAMWR);
    if (ret) return ret;
    
    /* Prepare one line of pixels */
    line_buffer = kmalloc(line_size * sizeof(u16), GFP_KERNEL); /* 9-bit words */
    if (!line_buffer)
        return -ENOMEM;
    
    /* Fill line buffer with color (each pixel = 2 bytes = High + Low) */
    for (i = 0; i < lcd->width; i++) {
        /* Each pixel needs 2 data bytes, sent as 9-bit words with D/C=1 */
        line_buffer[i * 2] = ili9488_pack9((color >> 8) & 0xFF, 1);   /* High byte */
        line_buffer[i * 2 + 1] = ili9488_pack9(color & 0xFF, 1);      /* Low byte */
    }
    
    /* Send all lines */
    for (i = 0; i < lcd->height; i++) {
        ret = spi_write(spi, line_buffer, line_size * 2); /* 2 bytes per 9-bit word */
        if (ret) break;
    }
    
    kfree(line_buffer);
    
    if (!ret)
        dev_info(&spi->dev, "Fill complete\n");
    
    return ret;
}

/* Convert RGB888 to RGB565 */
static u16 rgb888_to_rgb565(u8 r, u8 g, u8 b)
{
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

/* Sysfs: fill screen */
static ssize_t fill_store(struct device *dev, struct device_attribute *attr,
              const char *buf, size_t count)
{
    struct spi_device *spi = to_spi_device(dev);
    struct ili9488_dev *lcd = spi_get_drvdata(spi);
    u8 r, g, b;
    u16 color;
    
    if (!lcd || !lcd->power)
        return -ENODEV;
    
    /* Parse color: #RRGGBB or R G B */
    if (buf[0] == '#') {
        if (sscanf(buf + 1, "%2hhx%2hhx%2hhx", &r, &g, &b) != 3)
            return -EINVAL;
    } else {
        if (sscanf(buf, "%hhu %hhu %hhu", &r, &g, &b) != 3)
            return -EINVAL;
    }
    
    color = rgb888_to_rgb565(r, g, b);
    
    ili9488_fill_screen(lcd, color);
    return count;
}

/* Sysfs: raw command */
static ssize_t raw_store(struct device *dev, struct device_attribute *attr,
             const char *buf, size_t count)
{
    struct spi_device *spi = to_spi_device(dev);
    char op;
    u8 params[16];
    int num = 0;
    unsigned int val;
    const char *p = buf;
    int i, ret;
    
    /* Get operation type */
    while (*p && isspace(*p)) p++;
    if (!*p) return -EINVAL;
    
    op = *p++;
    
    /* Parse parameters */
    while (*p && num < 16) {
        while (*p && isspace(*p)) p++;
        if (!*p) break;
        
        if (sscanf(p, "%x", &val) != 1)
            break;
            
        params[num++] = (u8)val;
        
        while (*p && !isspace(*p)) p++;
    }
    
    if (op == 'C' || op == 'c') {
        /* Command with parameters */
        if (num == 0)
            return -EINVAL;
            
        ret = ili9488_cmd(spi, params[0]);
        if (ret) return ret;
        
        for (i = 1; i < num; i++) {
            ret = ili9488_data(spi, params[i]);
            if (ret) return ret;
        }
    } else if (op == 'D' || op == 'd') {
        /* Data only */
        for (i = 0; i < num; i++) {
            ret = ili9488_data(spi, params[i]);
            if (ret) return ret;
        }
    } else {
        return -EINVAL;
    }
    
    return count;
}

static DEVICE_ATTR_WO(fill);
static DEVICE_ATTR_WO(raw);

/* LCD operations */
static int ili9488_set_power(struct lcd_device *ld, int power)
{
    struct ili9488_dev *lcd = lcd_get_data(ld);
    struct spi_device *spi = lcd->spi;
    int ret = 0;
    
    dev_info(&spi->dev, "Power: %d -> %d\n", lcd->power, power);
    
    if (power == FB_BLANK_UNBLANK && !lcd->power) {
        /* Power ON */
        ret = ili9488_init_display(lcd);
        if (!ret) {
            lcd->power = 1;
            if (lcd->bl_gpiod) {
                gpiod_set_value_cansleep(lcd->bl_gpiod, 1);
                msleep(10);
            }
        }
    } else if (power != FB_BLANK_UNBLANK && lcd->power) {
        /* Power OFF */
        ili9488_cmd(spi, CMD_DISPLAY_OFF);
        if (lcd->bl_gpiod)
            gpiod_set_value_cansleep(lcd->bl_gpiod, 0);
        lcd->power = 0;
    }
    
    return ret;
}

static int ili9488_get_power(struct lcd_device *ld)
{
    struct ili9488_dev *lcd = lcd_get_data(ld);
    return lcd->power;
}

static struct lcd_ops ili9488_ops = {
    .set_power = ili9488_set_power,
    .get_power = ili9488_get_power,
};

/* Probe */
static int ili9488_probe(struct spi_device *spi)
{
    struct device *dev = &spi->dev;
    struct ili9488_dev *lcd;
    int ret;
    
    dev_info(dev, "Probing ILI9488 (3-line SPI)\n");
    
    lcd = devm_kzalloc(dev, sizeof(*lcd), GFP_KERNEL);
    if (!lcd)
        return -ENOMEM;
    
    lcd->spi = spi;
    spi_set_drvdata(spi, lcd);
    
    /* Get GPIOs */
    lcd->reset_gpiod = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_HIGH);
    if (IS_ERR(lcd->reset_gpiod))
        return PTR_ERR(lcd->reset_gpiod);
    
    lcd->bl_gpiod = devm_gpiod_get_optional(dev, "backlight", GPIOD_OUT_LOW);
    if (IS_ERR(lcd->bl_gpiod))
        return PTR_ERR(lcd->bl_gpiod);
    
    /* Get display parameters */
    lcd->width = 320;
    lcd->height = 480;
    of_property_read_u32(dev->of_node, "width", &lcd->width);
    of_property_read_u32(dev->of_node, "height", &lcd->height);
    
    /* Configure SPI for 9-bit mode */
    spi->mode = SPI_MODE_0;
    spi->bits_per_word = 9;
    ret = spi_setup(spi);
    if (ret) {
        dev_err(dev, "SPI setup failed: %d\n", ret);
        return ret;
    }
    
    dev_info(dev, "SPI: mode=%u, speed=%u, bits=%u\n",
             spi->mode, spi->max_speed_hz, spi->bits_per_word);
    
    /* Register LCD device */
    lcd->ld = devm_lcd_device_register(dev, "ili9488", dev, lcd, &ili9488_ops);
    if (IS_ERR(lcd->ld)) {
        dev_err(dev, "Failed to register LCD\n");
        return PTR_ERR(lcd->ld);
    }
    
    /* Create sysfs files */
    device_create_file(dev, &dev_attr_fill);
    device_create_file(dev, &dev_attr_raw);
    
    /* Power on display */
    ili9488_set_power(lcd->ld, FB_BLANK_UNBLANK);
    
    /* Test with red screen */
    msleep(100);
    ili9488_fill_screen(lcd, 0xF800); /* Red */
    
    dev_info(dev, "ILI9488 ready\n");
    return 0;
}

static int ili9488_remove(struct spi_device *spi)
{
    struct device *dev = &spi->dev;
    device_remove_file(dev, &dev_attr_fill);
    device_remove_file(dev, &dev_attr_raw);
    return 0;
}

static const struct of_device_id ili9488_of_match[] = {
    { .compatible = "ilitek,ili9488" },
    {}
};
MODULE_DEVICE_TABLE(of, ili9488_of_match);

static struct spi_driver ili9488_driver = {
    .driver = {
        .name = DRIVER_NAME,
        .of_match_table = ili9488_of_match,
    },
    .probe = ili9488_probe,
    .remove = ili9488_remove,
};

module_spi_driver(ili9488_driver);

MODULE_AUTHOR("tnv");
MODULE_DESCRIPTION("ILI9488 3-line SPI driver");
MODULE_LICENSE("GPL");
