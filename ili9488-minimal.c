/*
 * ili9488_backlight.c
 *
 * Minimal driver that binds to the ilitek,ili9488 device node (SPI)
 * and simply turns ON the backlight GPIO described by "backlight-gpios".
 *
 * - Uses descriptor API devm_gpiod_get_optional(..., "backlight", ...)
 * - Returns -EPROBE_DEFER if gpio-controller isn't ready
 * - Sets backlight to ON in probe and OFF in remove (if present)
 *
 * Target: Linux 4.9.x (works on newer kernels too)
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/spi/spi.h>
#include <linux/of.h>
#include <linux/gpio/consumer.h>

#define DRIVER_NAME "ili9488_backlight"

struct iback {
	struct spi_device *spi;
	struct gpio_desc *gpiod_bl;
	bool bl_on;
};

static int iback_probe(struct spi_device *spi)
{
	struct iback *ib;
	int ret = 0;

	dev_info(&spi->dev, DRIVER_NAME ": probe start\n");

	ib = devm_kzalloc(&spi->dev, sizeof(*ib), GFP_KERNEL);
	if (!ib)
		return -ENOMEM;

	ib->spi = spi;

	/* get optional backlight GPIO (property backlight-gpios in DT) */
	ib->gpiod_bl = devm_gpiod_get_optional(&spi->dev, "backlight", GPIOD_OUT_LOW);
	if (IS_ERR(ib->gpiod_bl)) {
		ret = PTR_ERR(ib->gpiod_bl);
		if (ret == -EPROBE_DEFER) {
			dev_warn(&spi->dev, DRIVER_NAME ": backlight gpiod not ready, deferring probe\n");
			return -EPROBE_DEFER;
		}
		dev_err(&spi->dev, DRIVER_NAME ": failed to get backlight gpiod: %d\n", ret);
		ib->gpiod_bl = NULL;
	}

	/* If backlight gpio present, switch it ON */
	if (ib->gpiod_bl) {
		/* Ensure gpio is low first (we requested OUT_LOW) then set high */
		gpiod_set_value_cansleep(ib->gpiod_bl, 1);
		ib->bl_on = true;
		dev_info(&spi->dev, DRIVER_NAME ": backlight set to ON\n");
	} else {
		dev_info(&spi->dev, DRIVER_NAME ": no backlight gpio defined in DT\n");
	}

	spi_set_drvdata(spi, ib);
	dev_info(&spi->dev, DRIVER_NAME ": probe ok\n");
	return 0;
}

static int iback_remove(struct spi_device *spi)
{
	struct iback *ib = spi_get_drvdata(spi);

	if (!ib)
		return 0;

	if (ib->gpiod_bl && ib->bl_on) {
		gpiod_set_value_cansleep(ib->gpiod_bl, 0);
		dev_info(&spi->dev, DRIVER_NAME ": backlight set to OFF on remove\n");
	}

	dev_info(&spi->dev, DRIVER_NAME ": removed\n");
	return 0;
}

static const struct of_device_id iback_of_match[] = {
	{ .compatible = "ilitek,ili9488" }, /* binds to same node as your ILI device */
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, iback_of_match);

static struct spi_driver iback_driver = {
	.driver = {
		.name = DRIVER_NAME,
		.of_match_table = iback_of_match,
	},
	.probe = iback_probe,
	.remove = iback_remove,
};

module_spi_driver(iback_driver);

MODULE_AUTHOR("Adapted-by-ChatGPT");
MODULE_DESCRIPTION("Minimal ILI9488 backlight enabler (turns 'backlight-gpios' ON)");
MODULE_LICENSE("GPL");
