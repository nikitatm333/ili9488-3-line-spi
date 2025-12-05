/*
 * ili9488_3bit.c - Драйвер для ILI9488 в 3-битном SPI режиме
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/spi/spi.h>
#include <linux/delay.h>
#include <linux/of.h>
#include <linux/gpio/consumer.h>

#define DRIVER_NAME "ili9488_3bit"

struct ili9488_dev {
    struct spi_device *spi;
    struct gpio_desc *reset_gpiod;
    struct gpio_desc *bl_gpiod;
};

/* Отправка 9-битного слова (D/C + 8-бит данных) через 2 байта */
static int ili9488_send_9bit(struct spi_device *spi, u8 dc, u8 data, const char *type)
{
    u8 tx_buf[2];
    struct spi_transfer t = {
        .tx_buf = tx_buf,
        .len = 2,
    };
    struct spi_message m;
    int ret;
    
    /* Формируем 9-битное слово как 2 байта:
     * 1-й байт: D/C + 7 старших бит данных
     * 2-й байт: 1 младший бит данных в старшей позиции
     */
    tx_buf[0] = (dc << 7) | (data >> 1);
    tx_buf[1] = (data & 0x01) << 7;
    
    spi_message_init(&m);
    spi_message_add_tail(&t, &m);
    
    pr_debug("ILI9488: %s: dc=%d, data=0x%02X -> buf[0]=0x%02X, buf[1]=0x%02X\n",
             type, dc, data, tx_buf[0], tx_buf[1]);
    
    ret = spi_sync(spi, &m);
    if (ret)
        pr_err("ILI9488: %s failed: %d\n", type, ret);
    
    return ret;
}

/* Отправка команды (D/C=0) */
static int ili9488_cmd(struct spi_device *spi, u8 cmd)
{
    return ili9488_send_9bit(spi, 0, cmd, "CMD");
}

/* Отправка данных (D/C=1) */
static int ili9488_data(struct spi_device *spi, u8 data)
{
    return ili9488_send_9bit(spi, 1, data, "DATA");
}

/* Отправка нескольких данных подряд */
static int ili9488_data_bulk(struct spi_device *spi, const u8 *data, size_t len)
{
    int i, ret = 0;
    
    for (i = 0; i < len; i++) {
        ret = ili9488_data(spi, data[i]);
        if (ret)
            break;
    }
    
    return ret;
}

/* Сброс дисплея через GPIO */
static void ili9488_hardware_reset(struct ili9488_dev *lcd)
{
    if (!lcd->reset_gpiod)
        return;
    
    pr_info("ILI9488: Hardware reset...\n");
    
    /* Активный низкий уровень - сброс */
    gpiod_set_value_cansleep(lcd->reset_gpiod, 0);
    msleep(20);
    gpiod_set_value_cansleep(lcd->reset_gpiod, 1);
    msleep(120); /* Обязательная задержка после сброса */
    
    pr_info("ILI9488: Hardware reset done\n");
}

/* Инициализация в 3-битном режиме (RGB 1-1-1) */
static void ili9488_3bit_init(struct ili9488_dev *lcd)
{
    struct spi_device *spi = lcd->spi;
    
    pr_info("ILI9488: Starting 3-bit mode initialization\n");
    
    /* 1. Аппаратный сброс */
    ili9488_hardware_reset(lcd);
    
    /* 2. Software Reset */
    ili9488_cmd(spi, 0x01); /* SWRESET */
    msleep(150);
    
    /* 3. Exit Sleep */
    ili9488_cmd(spi, 0x11); /* SLEEP OUT */
    msleep(120);
    
    /* 4. Interface Pixel Format - ВАЖНО: 3-bit/pixel для 3-line SPI */
    ili9488_cmd(spi, 0x3A); /* COLMOD */
    ili9488_data(spi, 0x01); /* 3-bit/pixel (RGB 1-1-1) - 8 цветов */
    msleep(10);
    
    /* 5. Memory Access Control */
    ili9488_cmd(spi, 0x36); /* MADCTL */
    ili9488_data(spi, 0x48); /* MY=0, MX=0, MV=0, ML=0, BGR=1, MH=0 */
    msleep(10);
    
    /* 6. Display Inversion ON */
    ili9488_cmd(spi, 0x21); /* Display Inversion ON */
    msleep(10);
    
    /* 7. Normal Display Mode ON */
    ili9488_cmd(spi, 0x13); /* NORON */
    msleep(10);
    
    /* 8. Display ON */
    ili9488_cmd(spi, 0x29); /* DISPON */
    msleep(50);
    
    pr_info("ILI9488: 3-bit mode initialization complete\n");
}

/* Цвета в 3-битном режиме (R[2], G[1], B[0]) */
#define COLOR_BLACK     0x00    /* 000 */
#define COLOR_BLUE      0x01    /* 001 */
#define COLOR_GREEN     0x02    /* 010 */
#define COLOR_CYAN      0x03    /* 011 */
#define COLOR_RED       0x04    /* 100 */
#define COLOR_MAGENTA   0x05    /* 101 */
#define COLOR_YELLOW    0x06    /* 110 */
#define COLOR_WHITE     0x07    /* 111 */

/* Заливка всего экрана указанным цветом (3-битный режим) */
static void ili9488_fill_color_3bit(struct spi_device *spi, u8 color)
{
    int total_pixels = 320 * 480;
    int i;
    
    pr_info("ILI9488: Заливка цветом 0x%02X (3-битный режим)...\n", color);
    
    /* Устанавливаем область рисования (весь экран 320x480) */
    ili9488_cmd(spi, 0x2A); /* CASET */
    ili9488_data(spi, 0x00); /* Start X high */
    ili9488_data(spi, 0x00); /* Start X low */
    ili9488_data(spi, 0x01); /* End X high */
    ili9488_data(spi, 0x3F); /* End X low (319 = 0x013F) */
    
    ili9488_cmd(spi, 0x2B); /* PASET */
    ili9488_data(spi, 0x00); /* Start Y high */
    ili9488_data(spi, 0x00); /* Start Y low */
    ili9488_data(spi, 0x01); /* End Y high */
    ili9488_data(spi, 0xDF); /* End Y low (479 = 0x01DF) */
    
    /* Команда начала записи в RAM */
    ili9488_cmd(spi, 0x2C); /* RAMWR */
    
    /* Отправляем все пиксели - 1 байт на пиксель в 3-битном режиме */
    for (i = 0; i < total_pixels; i++) {
        ili9488_data(spi, color);
        
        /* Прогресс каждые 5000 пикселей */
        if (i % 5000 == 0 && i > 0)
            pr_info("ILI9488: Заполнено %d пикселей из %d\n", i, total_pixels);
    }
    
    pr_info("ILI9488: Заливка завершена\n");
}

/* Probe функция */
static int ili9488_3bit_probe(struct spi_device *spi)
{
    struct device *dev = &spi->dev;
    struct ili9488_dev *lcd;
    int ret;
    
    pr_info("ILI9488: Загружаю драйвер (3-битный режим, IM[2:0]=101)\n");
    
    lcd = devm_kzalloc(dev, sizeof(*lcd), GFP_KERNEL);
    if (!lcd)
        return -ENOMEM;
    
    lcd->spi = spi;
    spi_set_drvdata(spi, lcd);
    
    /* Получаем GPIO */
    lcd->reset_gpiod = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_LOW);
    if (IS_ERR(lcd->reset_gpiod)) {
        dev_err(dev, "Не могу получить GPIO сброса\n");
        return PTR_ERR(lcd->reset_gpiod);
    }
    
    lcd->bl_gpiod = devm_gpiod_get_optional(dev, "backlight", GPIOD_OUT_LOW);
    if (IS_ERR(lcd->bl_gpiod)) {
        dev_err(dev, "Не могу получить GPIO подсветки\n");
        return PTR_ERR(lcd->bl_gpiod);
    }
    
    /* Настраиваем SPI */
    spi->mode = SPI_MODE_3; /* CPOL=1, CPHA=1 */
    spi->bits_per_word = 8; /* 8-битный SPI, но отправляем 2 байта для 9 бит */
    spi->max_speed_hz = 1000000; /* 1 МГц для надежности в 3-битном режиме */
    
    ret = spi_setup(spi);
    if (ret) {
        dev_err(dev, "Ошибка настройки SPI: %d\n", ret);
        return ret;
    }
    
    pr_info("ILI9488: SPI настроен: mode=3, speed=%u, bits=%u\n", 
            spi->max_speed_hz, spi->bits_per_word);
    
    /* Включаем подсветку если есть */
    if (lcd->bl_gpiod) {
        gpiod_set_value_cansleep(lcd->bl_gpiod, 1);
        msleep(10);
        pr_info("ILI9488: Подсветка включена\n");
    }
    
    /* Инициализация в 3-битном режиме */
    ili9488_3bit_init(lcd);
    
    /* Тест разных цветов */
    msleep(100);
    
    pr_info("ILI9488: Тест красного цвета...\n");
    ili9488_fill_color_3bit(spi, COLOR_RED); /* Красный */
    msleep(2000);
    
    pr_info("ILI9488: Тест зеленого цвета...\n");
    ili9488_fill_color_3bit(spi, COLOR_GREEN); /* Зеленый */
    msleep(2000);
    
    pr_info("ILI9488: Тест синего цвета...\n");
    ili9488_fill_color_3bit(spi, COLOR_BLUE); /* Синий */
    msleep(2000);
    
    pr_info("ILI9488: Тест белого цвета...\n");
    ili9488_fill_color_3bit(spi, COLOR_WHITE); /* Белый */
    msleep(2000);
    
    pr_info("ILI9488: Тест черного цвета...\n");
    ili9488_fill_color_3bit(spi, COLOR_BLACK); /* Черный */
    
    pr_info("ILI9488: Драйвер успешно загружен!\n");
    return 0;
}

static int ili9488_3bit_remove(struct spi_device *spi)
{
    struct ili9488_dev *lcd = spi_get_drvdata(spi);
    
    if (lcd && lcd->bl_gpiod) {
        gpiod_set_value_cansleep(lcd->bl_gpiod, 0);
        pr_info("ILI9488: Подсветка выключена\n");
    }
    
    pr_info("ILI9488: Драйвер выгружен\n");
    return 0;
}

static const struct of_device_id ili9488_3bit_of_match[] = {
    { .compatible = "ilitek,ili9488" },
    { .compatible = "ili9488" },
    {},
};
MODULE_DEVICE_TABLE(of, ili9488_3bit_of_match);

static struct spi_driver ili9488_3bit_driver = {
    .driver = {
        .name = DRIVER_NAME,
        .of_match_table = ili9488_3bit_of_match,
        .owner = THIS_MODULE,
    },
    .probe = ili9488_3bit_probe,
    .remove = ili9488_3bit_remove,
};

module_spi_driver(ili9488_3bit_driver);

MODULE_AUTHOR("tnv");
MODULE_DESCRIPTION("Драйвер ILI9488 в 3-битном SPI режиме (8 цветов)");
MODULE_LICENSE("GPL");
