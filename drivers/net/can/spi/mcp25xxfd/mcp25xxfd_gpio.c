// SPDX-License-Identifier: GPL-2.0

/* CAN bus driver for Microchip 25XXFD CAN Controller with SPI Interface
 *
 * Copyright 2019 Martin Sperl <kernel@martin.sperl.org>
 *
 * Based on Microchip MCP251x CAN controller driver written by
 * David Vrabel, Copyright 2006 Arcom Control Systems Ltd.
 */

#include <linux/gpio/driver.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>

#include "mcp25xxfd_clock.h"
#include "mcp25xxfd_cmd.h"
#include "mcp25xxfd_gpio.h"
#include "mcp25xxfd_priv.h"

static int mcp25xxfd_gpio_request(struct gpio_chip *chip,
				  unsigned int offset)
{
	struct mcp25xxfd_priv *priv = gpiochip_get_data(chip);
	int clock_requestor = offset ?
		MCP25XXFD_CLK_USER_GPIO1 : MCP25XXFD_CLK_USER_GPIO0;

	/* only handle gpio 0/1 */
	if (offset > 1)
		return -EINVAL;

	/* if we have XSTANDBY enabled then gpio0 is not available either */
	if (priv->config.gpio0_xstandby && offset == 0)
		return -EINVAL;

	mcp25xxfd_clock_start(priv, clock_requestor);

	return 0;
}

static void mcp25xxfd_gpio_free(struct gpio_chip *chip,
				unsigned int offset)
{
	struct mcp25xxfd_priv *priv = gpiochip_get_data(chip);
	int clock_requestor = offset ?
		MCP25XXFD_CLK_USER_GPIO1 : MCP25XXFD_CLK_USER_GPIO0;

	/* only handle gpio 0/1 */
	if (offset > 1)
		return;

	mcp25xxfd_clock_stop(priv, clock_requestor);
}

static int mcp25xxfd_gpio_get(struct gpio_chip *chip, unsigned int offset)
{
	struct mcp25xxfd_priv *priv = gpiochip_get_data(chip);
	u32 mask = (offset) ? MCP25XXFD_IOCON_GPIO1 : MCP25XXFD_IOCON_GPIO0;
	int ret;

	/* only handle gpio 0/1 */
	if (offset > 1)
		return -EINVAL;

	/* read the relevant gpio Latch */
	ret = mcp25xxfd_cmd_read_mask(priv->spi, MCP25XXFD_IOCON,
				      &priv->regs.iocon, mask);
	if (ret)
		return ret;

	/* return the match */
	return priv->regs.iocon & mask;
}

static void mcp25xxfd_gpio_set(struct gpio_chip *chip, unsigned int offset,
			       int value)
{
	struct mcp25xxfd_priv *priv = gpiochip_get_data(chip);
	u32 mask = (offset) ? MCP25XXFD_IOCON_LAT1 : MCP25XXFD_IOCON_LAT0;

	/* only handle gpio 0/1 */
	if (offset > 1)
		return;

	/* update in memory representation with the corresponding value */
	if (value)
		priv->regs.iocon |= mask;
	else
		priv->regs.iocon &= ~mask;

	mcp25xxfd_cmd_write_mask(priv->spi, MCP25XXFD_IOCON,
				 priv->regs.iocon, mask);
}

static int mcp25xxfd_gpio_direction_input(struct gpio_chip *chip,
					  unsigned int offset)
{
	struct mcp25xxfd_priv *priv = gpiochip_get_data(chip);
	u32 mask_tri = (offset) ?
		MCP25XXFD_IOCON_TRIS1 : MCP25XXFD_IOCON_TRIS0;
	u32 mask_stby = (offset) ?
		0 : MCP25XXFD_IOCON_XSTBYEN;
	u32 mask_pm = (offset) ?
		MCP25XXFD_IOCON_PM1 : MCP25XXFD_IOCON_PM0;

	/* only handle gpio 0/1 */
	if (offset > 1)
		return -EINVAL;

	/* set the mask */
	priv->regs.iocon |= mask_tri | mask_pm;

	/* clear stby */
	priv->regs.iocon &= ~mask_stby;

	return mcp25xxfd_cmd_write_mask(priv->spi, MCP25XXFD_IOCON,
					priv->regs.iocon,
					mask_tri | mask_stby | mask_pm);
}

static int mcp25xxfd_gpio_direction_output(struct gpio_chip *chip,
					   unsigned int offset, int value)
{
	struct mcp25xxfd_priv *priv = gpiochip_get_data(chip);
	u32 mask_tri = (offset) ?
		MCP25XXFD_IOCON_TRIS1 : MCP25XXFD_IOCON_TRIS0;
	u32 mask_lat = (offset) ?
		MCP25XXFD_IOCON_LAT1 : MCP25XXFD_IOCON_LAT0;
	u32 mask_pm = (offset) ?
		MCP25XXFD_IOCON_PM1 : MCP25XXFD_IOCON_PM0;
	u32 mask_stby = (offset) ?
		0 : MCP25XXFD_IOCON_XSTBYEN;

	/* only handle gpio 0/1 */
	if (offset > 1)
		return -EINVAL;

	/* clear the tristate bit and also clear stby */
	priv->regs.iocon &= ~(mask_tri | mask_stby);

	/* set GPIO mode */
	priv->regs.iocon |= mask_pm;

	/* set the value */
	if (value)
		priv->regs.iocon |= mask_lat;
	else
		priv->regs.iocon &= ~mask_lat;

	return mcp25xxfd_cmd_write_mask(priv->spi, MCP25XXFD_IOCON,
					priv->regs.iocon,
					mask_tri | mask_lat |
					mask_pm | mask_stby);
}

#ifdef CONFIG_OF_DYNAMIC
static void mcp25xxfd_gpio_read_of(struct mcp25xxfd_priv *priv)
{
	const struct device_node *np = priv->spi->dev.of_node;

	priv->config.gpio_open_drain =
		of_property_read_bool(np, "microchip,gpio-open-drain");
	priv->config.gpio0_xstandby =
		of_property_read_bool(np, "microchip,gpio0-xstandby");
}
#else
static void mcp25xxfd_gpio_read_of(struct mcp25xxfd_priv *priv)
{
	priv->config.gpio_open_drain = false;
	priv->config.gpio0_xstandby = false;
}
#endif

static int mcp25xxfd_gpio_setup_regs(struct mcp25xxfd_priv *priv)
{
	/* handle open-drain */
	if (priv->config.gpio_open_drain)
		priv->regs.iocon |= MCP25XXFD_IOCON_INTOD;
	else
		priv->regs.iocon &= ~MCP25XXFD_IOCON_INTOD;

	/* handle xstandby */
	if (priv->config.gpio0_xstandby) {
		priv->regs.iocon &= ~(MCP25XXFD_IOCON_TRIS0 |
				      MCP25XXFD_IOCON_GPIO0);
		priv->regs.iocon |= MCP25XXFD_IOCON_XSTBYEN;
	} else {
		priv->regs.iocon &= ~(MCP25XXFD_IOCON_XSTBYEN);
	}

	/* handle sof / clko */
	if (priv->config.clock_odiv == 0)
		priv->regs.iocon |= MCP25XXFD_IOCON_SOF;
	else
		priv->regs.iocon &= ~MCP25XXFD_IOCON_SOF;

	/* update the iocon register */
	return mcp25xxfd_cmd_write_regs(priv->spi, MCP25XXFD_IOCON,
					&priv->regs.iocon, sizeof(u32));
}

static int mcp25xxfd_gpio_setup_gpiochip(struct mcp25xxfd_priv *priv)
{
	struct gpio_chip *gpio = &priv->gpio;

	/* gpiochip only handles GPIO0 and GPIO1 */
	gpio->owner                = THIS_MODULE;
	gpio->parent               = &priv->spi->dev;
	gpio->label                = dev_name(&priv->spi->dev);
	gpio->direction_input      = mcp25xxfd_gpio_direction_input;
	gpio->get                  = mcp25xxfd_gpio_get;
	gpio->direction_output     = mcp25xxfd_gpio_direction_output;
	gpio->set                  = mcp25xxfd_gpio_set;
	gpio->request              = mcp25xxfd_gpio_request;
	gpio->free                 = mcp25xxfd_gpio_free;
	gpio->base                 = -1;
	gpio->ngpio                = 2;
	gpio->can_sleep            = 1;

	return gpiochip_add_data(gpio, priv);
}

int mcp25xxfd_gpio_setup(struct mcp25xxfd_priv *priv)
{
	int ret;

	/* setting up defaults */
	priv->config.gpio0_xstandby = false;

	mcp25xxfd_gpio_read_of(priv);
	ret = mcp25xxfd_gpio_setup_regs(priv);
	if (ret)
		return ret;

	return mcp25xxfd_gpio_setup_gpiochip(priv);
}

void mcp25xxfd_gpio_remove(struct mcp25xxfd_priv *priv)
{
	gpiochip_remove(&priv->gpio);
}
