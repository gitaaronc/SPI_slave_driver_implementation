/*
 * Driver for OMAP2 McSPI controller in slave mode.
 *
 * Copyright (C) 2016 Patryk Mężydło <mezydlo.p@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/device.h>
#include <linux/io.h>
#include <linux/pm_runtime.h>
#include <linux/of_irq.h>
#include <linux/interrupt.h>
#include <linux/uaccess.h>

#define DRIVER_NAME "spi-mcspi-slave"

#include <linux/platform_data/spi-omap2-mcspi.h>

#include "spi-slave-core.h"

#define MCSPI_PIN_DIR_D0_IN_D1_OUT		0
#define MCSPI_PIN_DIR_D0_OUT_D1_IN		1
#define MCSPI_CS_POLARITY_ACTIVE_HIGH		1
#define MCSPI_CS_POLARITY_ACTIVE_LOW		0
#define MCSPI_CS_SENSITIVE_ENABLED		1
#define MCSPI_CS_SENSITIVE_DISABLED		0
#define MCSPI_MAX_FIFO_DEPTH			64
#define MCSPI_POL_HELD_HIGH			0
#define MCSPI_POL_HELD_LOW			1
#define MCSPI_PHA_ODD_NUMBERED_EDGES		0
#define MCSPI_PHA_EVEN_NUMBERED_EDGES		1

#define MCSPI_MODE_TRM				0
#define MCSPI_MODE_RM				1
#define MCSPI_MODE_TM				2

#define SPI_SLAVE_BUF_DEPTH			64
#define SPI_SLAVE_BITS_PER_WORD			8
#define SPII_SLAVE_CS_SENSITIVE			MCSPI_CS_SENSITIVE_ENABLED
#define SPI_SLAVE_CS_POLARITY			MCSPI_CS_POLARITY_ACTIVE_LOW
#define SPI_SLAVE_PIN_DIR			MCSPI_PIN_DIR_D0_IN_D1_OUT
#define SPI_SLAVE_MODE				MCSPI_MODE_TRM
#define SPI_SLAVE_COPY_LENGTH			1

#define MCSPI_SYSCONFIG				0x10
#define MCSPI_SYSSTATUS				0x14
#define MCSPI_IRQSTATUS				0x18
#define MCSPI_IRQENABLE				0x1C
#define MCSPI_SYST				0x24
#define MCSPI_MODULCTRL				0x28
#define MCSPI_CH0CONF				0x2C
#define MCSPI_CH0STAT				0x30
#define MCSPI_CH0CTRL				0x34
#define MCSPI_TX0				0x38
#define MCSPI_RX0				0x3C
#define MCSPI_XFERLEVEL				0x7C
#define MCSPI_DAFTX				0x80
#define MCSPI_DAFRX				0xA0

#define SPI_AUTOSUSPEND_TIMEOUT			-1

#define MCSPI_SYSSTATUS_RESETDONE		BIT(0)
#define MCSPI_MODULCTRL_MS			BIT(2)
#define MCSPI_MODULCTRL_PIN34			BIT(1)
#define MCSPI_CHCTRL_EN				BIT(0)
#define MCSPI_CHCONF_EPOL			BIT(6)

#define MCSPI_CHCONF_TRM			(0x03 << 12)
#define MCSPI_CHCONF_TM				BIT(13)
#define MCSPI_CHCONF_RM				BIT(12)

#define MCSPI_CHCONF_WL				(0x1F << 7)

#define MCSPI_CHCONF_WL_8BIT_MASK		(0x07 << 7)
#define MCSPI_CHCONF_WL_16BIT_MASK		(0x0F << 7)
#define MCSPI_CHCONF_WL_32BIT_MASK		(0x1F << 7)

#define MCSPI_CHCONF_IS				BIT(18)
#define MCSPI_CHCONF_DPE0			BIT(16)
#define MCSPI_CHCONF_DPE1			BIT(17)
#define MCSPI_CHCONF_POL			BIT(1)
#define MCSPI_CHCONF_PHA			BIT(0)

#define MCSPI_IRQ_RX_OVERFLOW			BIT(3)
#define MCSPI_IRQ_RX_FULL			BIT(2)
#define MCSPI_IRQ_TX_UNDERFLOW			BIT(1)
#define MCSPI_IRQ_TX_EMPTY			BIT(0)
#define MCSPI_IRQ_EOW				BIT(17)

#define MCSPI_SYSCONFIG_CLOCKACTIVITY		(0x03 << 8)
#define MCSPI_SYSCONFIG_SIDLEMODE		(0x03 << 3)
#define MCSPI_SYSCONFIG_SOFTRESET		BIT(1)
#define MCSPI_SYSCONFIG_AUTOIDLE		BIT(0)

#define MCSPI_IRQ_RESET				0xFFFFFFFF

#define MCSPI_XFER_AFL				(0x7 << 8)
#define MCSPI_XFER_AEL				(0x7)
#define MCSPI_XFER_WCNT				(0xFFFF << 16)

#define MCSPI_CHCONF_FFER			BIT(28)
#define MCSPI_CHCONF_FFEW			BIT(27)

#define MCSPI_MODULCTRL_MOA			BIT(7)
#define MCSPI_MODULCTRL_FDAA			BIT(8)

#define MCSPI_CHSTAT_EOT			BIT(2)
#define MCSPI_CHSTAT_TXS			BIT(1)
#define MCSPI_CHSTAT_RXS			BIT(0)
#define MCSPI_CHSTAT_RXFFF			BIT(6)
#define MCSPI_CHSTAT_RXFFE			BIT(5)
#define MCSPI_CHSTAT_TXFFF			BIT(4)
#define MCSPI_CHSTAT_TXFFE			BIT(3)

unsigned int mcspi_slave_read_reg(void __iomem *base, u32 idx)
{
	return ioread32(base + idx);
}

void mcspi_slave_write_reg(void __iomem *base,
		u32 idx, u32 val)
{
	iowrite32(val, base + idx);
}

int mcspi_slave_bytes_per_word(int word_len)
{
	if (word_len <= 8)
		return 1;
	else if (word_len <= 16)
		return 2;
	else
		return 4;
}

int mcspi_slave_wait_for_bit(void __iomem *reg, u32 bit)
{
	unsigned long timeout;

	timeout = jiffies + msecs_to_jiffies(1000);
	while (!(ioread32(reg) & bit)) {
		if (time_after(jiffies, timeout))
			return -ETIMEDOUT;
		else
			return 0;

		cpu_relax();
	}
	return 0;
}

void mcspi_slave_enable(struct spi_slave *slave)
{
	u32 val;

	val = mcspi_slave_read_reg(slave->base, MCSPI_CH0CTRL);
	val |= MCSPI_CHCTRL_EN;
	mcspi_slave_write_reg(slave->base, MCSPI_CH0CTRL, val);
}

void mcspi_slave_disable(struct spi_slave *slave)
{
	u32 val;

	val = mcspi_slave_read_reg(slave->base, MCSPI_CH0CTRL);
	val &= ~MCSPI_CHCTRL_EN;
	mcspi_slave_write_reg(slave->base, MCSPI_CH0CTRL, val);
}

void mcspi_slave_pio_rx_transfer(unsigned long data)
{
	struct spi_slave *slave;
	unsigned int c;
	void __iomem *rx_reg;
	void __iomem *chstat;

	slave = (struct spi_slave *) data;

	rx_reg = slave->base + MCSPI_RX0;
	chstat = slave->base + MCSPI_CH0STAT;

	c = slave->bytes_per_load;
	c /= mcspi_slave_bytes_per_word(slave->bits_per_word);

	if (slave->rx_offset >= slave->buf_depth) {
		dev_dbg(&slave->dev, "end of rx buffer!\n");
		slave->rx_offset = 0;
		return;
	}

	switch (mcspi_slave_bytes_per_word(slave->bits_per_word)) {
	case 1: {
		u8 *rx;

		rx = slave->rx + slave->rx_offset;
		slave->rx_offset += (sizeof(u8) * c);

		do {
			c -= 1;
			if (mcspi_slave_wait_for_bit(chstat, MCSPI_CHSTAT_RXS)
						     < 0)
				goto out;

			*rx++ = readl_relaxed(rx_reg);
		} while (c);
	} break;

	case  2: {
		u16 *rx;

		rx = slave->rx + slave->rx_offset;
		slave->rx_offset += (sizeof(u16) * c);

		do {
			c -= 1;
			if (mcspi_slave_wait_for_bit(chstat, MCSPI_CHSTAT_RXS)
						     < 0)
				goto out;

			*rx++ = readl_relaxed(rx_reg);
		} while (c);
	} break;

	case 4: {
		u32 *rx;

		rx = slave->rx + slave->rx_offset;
		slave->rx_offset += (sizeof(u32) * c);

		do {
			c -= 1;
			if (mcspi_slave_wait_for_bit(chstat, MCSPI_CHSTAT_RXS)
						     < 0)
				goto out;

			*rx++ = readl_relaxed(rx_reg);
		} while (c);
	} break;

	default:
		return;
	}

	return;
out:
	dev_dbg(&slave->dev, "timeout!\n");
}
DECLARE_TASKLET(pio_rx_tasklet, mcspi_slave_pio_rx_transfer, 0);

void mcspi_slave_pio_tx_transfer(struct spi_slave *slave)
{
	unsigned int c;
	void __iomem *tx_reg;
	void __iomem *chstat;

	tx_reg = slave->base + MCSPI_TX0;
	chstat = slave->base + MCSPI_CH0STAT;

	if (slave->mode == MCSPI_MODE_TM)
		c = MCSPI_MAX_FIFO_DEPTH;
	else
		c = MCSPI_MAX_FIFO_DEPTH / 2;

	c /= mcspi_slave_bytes_per_word(slave->bits_per_word);

	if (slave->tx_offset >= slave->buf_depth) {
		dev_dbg(&slave->dev, "end of tx buffer!\n");
		slave->tx_offset = 0;
		return;
	}

	switch (mcspi_slave_bytes_per_word(slave->bits_per_word)) {
	case 1: {
		const u8 *tx;

		tx = slave->tx + slave->tx_offset;
		slave->tx_offset += (sizeof(u8) * c);

		do {
			c -= 1;
			if (mcspi_slave_wait_for_bit(chstat, MCSPI_CHSTAT_TXS)
						     < 0)
				goto out;

			writel_relaxed(*tx++, tx_reg);
		} while (c);
	} break;

	case 2: {
		const u16 *tx;

		tx = slave->tx + slave->tx_offset;
		slave->tx_offset += (sizeof(u16) * c);

		do {
			c -= 1;
			if (mcspi_slave_wait_for_bit(chstat, MCSPI_CHSTAT_TXS)
						     < 0)
				goto out;

			writel_relaxed(*tx++, tx_reg);
		} while (c);
	} break;

	case 4: {
		const u32 *tx;

		tx = slave->tx + slave->tx_offset;
		slave->tx_offset += (sizeof(u32) * c);

		do {
			c -= 1;
			if (mcspi_slave_wait_for_bit(chstat, MCSPI_CHSTAT_TXS)
						     < 0)
				goto out;

			writel_relaxed(*tx++, tx_reg);
		} while (c);
	} break;

	default:
		return;
	}

	return;
out:
	dev_dbg(&slave->dev, "timeout!!!\n");
}

irq_handler_t mcspi_slave_irq(unsigned int irq, void *dev_id)
{
	struct spi_slave *slave = dev_id;
	u32 val;
	unsigned long flags;

	val = mcspi_slave_read_reg(slave->base, MCSPI_CH0STAT);

	if (val & MCSPI_CHSTAT_EOT) {
		spin_lock_irqsave(&slave->wait_lock, flags);
		wake_up_all(&slave->wait);
		spin_unlock_irqrestore(&slave->wait_lock, flags);
		mcspi_slave_disable(slave);
	}

	val = mcspi_slave_read_reg(slave->base, MCSPI_IRQSTATUS);

	if (val & MCSPI_IRQ_RX_FULL) {
		val |= MCSPI_IRQ_RX_FULL;
		pio_rx_tasklet.data = (unsigned long)slave;
		tasklet_schedule(&pio_rx_tasklet);
	}

	mcspi_slave_write_reg(slave->base, MCSPI_IRQSTATUS, val);
	return (irq_handler_t) IRQ_HANDLED;
}

int mcspi_slave_set_irq(struct spi_slave *slave)
{
	u32 val;
	int ret;

	val = mcspi_slave_read_reg(slave->base, MCSPI_IRQENABLE);

	val &= ~MCSPI_IRQ_RX_FULL;
	val &= ~MCSPI_IRQ_TX_EMPTY;
	val |= MCSPI_IRQ_RX_FULL;

	mcspi_slave_write_reg(slave->base, MCSPI_IRQENABLE, val);

	ret = devm_request_irq(&slave->dev, slave->irq,
				(irq_handler_t)mcspi_slave_irq,
				IRQF_TRIGGER_NONE,
				DRIVER_NAME, slave);
	if (ret) {
		dev_dbg(&slave->dev, "unable to request irq:%d\n", slave->irq);
		return -EINTR;
	}

	return 0;
}

int mcspi_slave_setup_pio_transfer(struct spi_slave *slave)
{
	u32 val;

	if (slave->mode == MCSPI_MODE_TM || slave->mode == MCSPI_MODE_TRM) {
		slave->tx = kzalloc(slave->buf_depth, GFP_KERNEL);
		if (slave->tx == NULL)
			return -ENOMEM;
	}

	if (slave->mode == MCSPI_MODE_RM || slave->mode == MCSPI_MODE_TRM) {
		slave->rx = kzalloc(slave->buf_depth, GFP_KERNEL);
		if (slave->rx == NULL)
			return -ENOMEM;
	}

	val = mcspi_slave_read_reg(slave->base, MCSPI_XFERLEVEL);

	val &= ~MCSPI_XFER_AEL;
	val &= ~MCSPI_XFER_AFL;

	if (slave->mode == MCSPI_MODE_RM || slave->mode == MCSPI_MODE_TRM)
		val  |= (slave->bytes_per_load - 1) << 8;

	val &= ~MCSPI_XFER_WCNT;

	mcspi_slave_write_reg(slave->base, MCSPI_XFERLEVEL, val);

	val = mcspi_slave_read_reg(slave->base, MCSPI_CH0CONF);

	val &= ~MCSPI_CHCONF_TRM;

	if (slave->mode == MCSPI_MODE_RM)
		val |= MCSPI_CHCONF_RM;
	else if (slave->mode == MCSPI_MODE_TM)
		val |= MCSPI_CHCONF_TM;

	val &= ~MCSPI_CHCONF_WL;
	val |= (slave->bits_per_word - 1) << 7;

	val &= ~MCSPI_CHCONF_FFER;
	val &= ~MCSPI_CHCONF_FFEW;

	if (slave->mode == MCSPI_MODE_RM || slave->mode == MCSPI_MODE_TRM)
		val |= MCSPI_CHCONF_FFER;

	if (slave->mode == MCSPI_MODE_TM || slave->mode == MCSPI_MODE_TRM)
		val |= MCSPI_CHCONF_FFEW;

	mcspi_slave_write_reg(slave->base, MCSPI_CH0CONF, val);
	val = mcspi_slave_read_reg(slave->base, MCSPI_MODULCTRL);
	val &= ~MCSPI_MODULCTRL_FDAA;
	mcspi_slave_write_reg(slave->base, MCSPI_MODULCTRL, val);

	return 0;
}

void mcspi_slave_clr_pio_transfer(struct spi_slave *slave)
{
	if (slave->tx != NULL)
		kfree(slave->tx);

	if (slave->rx != NULL)
		kfree(slave->rx);

	mcspi_slave_disable(slave);
}

void mcspi_slave_set_slave_mode(struct spi_slave *slave)
{
	u32 val;

	val = mcspi_slave_read_reg(slave->base, MCSPI_MODULCTRL);

	val |= MCSPI_MODULCTRL_MS;

	mcspi_slave_write_reg(slave->base, MCSPI_MODULCTRL, val);

	val = mcspi_slave_read_reg(slave->base, MCSPI_CH0CONF);

	val &= ~MCSPI_CHCONF_PHA;
	val &= ~MCSPI_CHCONF_POL;

	if (slave->pin_dir == MCSPI_PIN_DIR_D0_IN_D1_OUT) {
		val &= ~MCSPI_CHCONF_IS;
		val &= ~MCSPI_CHCONF_DPE1;
		val |= MCSPI_CHCONF_DPE0;
	} else {
		val |= MCSPI_CHCONF_IS;
		val |= MCSPI_CHCONF_DPE1;
		val &= ~MCSPI_CHCONF_DPE0;
	}

	if (slave->pol == MCSPI_POL_HELD_HIGH)
		val &= ~MCSPI_CHCONF_POL;
	else
		val |= MCSPI_CHCONF_POL;

	if (slave->pha == MCSPI_PHA_ODD_NUMBERED_EDGES)
		val &= ~MCSPI_CHCONF_PHA;
	else
		val |= MCSPI_CHCONF_PHA;

	mcspi_slave_write_reg(slave->base, MCSPI_CH0CONF, val);
}

void mcspi_slave_set_cs(struct spi_slave *slave)
{
	u32 val;

	val = mcspi_slave_read_reg(slave->base, MCSPI_CH0CONF);

	if (slave->cs_polarity == MCSPI_CS_POLARITY_ACTIVE_LOW)
		val |= MCSPI_CHCONF_EPOL;
	else
		val &= ~MCSPI_CHCONF_EPOL;

	mcspi_slave_write_reg(slave->base, MCSPI_CH0CONF, val);
	val = mcspi_slave_read_reg(slave->base, MCSPI_MODULCTRL);

	if (slave->cs_sensitive == MCSPI_CS_SENSITIVE_ENABLED)
		val &= ~MCSPI_MODULCTRL_PIN34;
	else
		val |= MCSPI_MODULCTRL_PIN34;

	mcspi_slave_write_reg(slave->base, MCSPI_MODULCTRL, val);
}

int mcspi_slave_setup(struct spi_slave *slave)
{
	int ret = 0;

	if (mcspi_slave_wait_for_bit(slave->base + MCSPI_SYSSTATUS,
					      MCSPI_SYSSTATUS_RESETDONE) != 0) {
		dev_dbg(&slave->dev, "internal module reset is on-going\n");
		return -EIO;
	}

		mcspi_slave_disable(slave);
		mcspi_slave_set_slave_mode(slave);
		mcspi_slave_set_cs(slave);
		ret = mcspi_slave_set_irq(slave);

		if (ret < 0)
			return -EINTR;

	return ret;
}

void mcspi_slave_clean_up(struct spi_slave *slave)
{
	tasklet_kill(&pio_rx_tasklet);

	if (slave->tx != NULL)
		kfree(slave->tx);

	if (slave->rx != NULL)
		kfree(slave->rx);
}

void mcspi_slave_enable_transfer(struct spi_slave *slave)
{
	slave->rx_offset = 0;
	memset(slave->rx, 0, slave->buf_depth);
	slave->tx_offset = 0;
	mcspi_slave_enable(slave);
	mcspi_slave_pio_tx_transfer(slave);
}

struct omap2_mcspi_platform_config mcspi_slave_pdata = {
	.regs_offset	= OMAP4_MCSPI_REG_OFFSET,
};

const struct of_device_id mcspi_slave_of_match[] = {
	{
		.compatible = "ti,omap4-mcspi-slave",
		.data = &mcspi_slave_pdata,
	},
	{ }
};
MODULE_DEVICE_TABLE(of, mcspi_slave_of_match);

static int mcspi_slave_probe(struct platform_device *pdev)
{
	struct resource	*res;
	struct resource	cp_res;
	const struct of_device_id *match;
	const struct omap2_mcspi_platform_config *pdata;
	int ret = 0;
	u32 regs_offset = 0;
	struct spi_slave *slave;
	u32 cs_sensitive;
	u32 cs_polarity;
	unsigned int pin_dir;
	unsigned int irq;
	unsigned int pha;
	unsigned int pol;

	slave = spislave_alloc_slave(&pdev->dev, sizeof(struct spi_slave));
	if (slave == NULL)
		return -ENOMEM;

	match = of_match_device(mcspi_slave_of_match, &pdev->dev);

	if (!match) {
		dev_dbg(&slave->dev, "failed to match, install DTS\n");
		ret = -EINVAL;
		goto free_slave;
	}

	pdata = match->data;

	if (of_get_property(pdev->dev.of_node, "cs_polarity", &cs_polarity))
		cs_polarity = MCSPI_CS_POLARITY_ACTIVE_HIGH;
	else
		cs_polarity = MCSPI_CS_POLARITY_ACTIVE_LOW;

	if (of_get_property(pdev->dev.of_node, "cs_sensitive", &cs_sensitive))
		cs_sensitive = MCSPI_CS_SENSITIVE_DISABLED;
	else
		cs_sensitive = MCSPI_CS_SENSITIVE_ENABLED;

	if (of_get_property(pdev->dev.of_node, "pindir-D0-out-D1-in", &pin_dir))
		pin_dir = MCSPI_PIN_DIR_D0_OUT_D1_IN;
	else
		pin_dir = MCSPI_PIN_DIR_D0_IN_D1_OUT;

	if (of_get_property(pdev->dev.of_node, "pha", &pha))
		pha = MCSPI_PHA_EVEN_NUMBERED_EDGES;
	else
		pha = MCSPI_PHA_ODD_NUMBERED_EDGES;

	if (of_get_property(pdev->dev.of_node, "pol", &pol))
		pol = MCSPI_POL_HELD_LOW;
	else
		pol = MCSPI_POL_HELD_HIGH;

	irq = irq_of_parse_and_map(pdev->dev.of_node, 0);
	regs_offset = pdata->regs_offset;
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	memcpy(&cp_res, res, sizeof(struct resource));

	if (res == NULL) {
		dev_dbg(&slave->dev, "res not availablee\n");
		ret = -ENODEV;
		goto free_slave;
	}

	cp_res.start += regs_offset;
	cp_res.end   += regs_offset;

	slave->base = devm_ioremap_resource(&pdev->dev, &cp_res);

	if (IS_ERR(slave->base)) {
		dev_dbg(&slave->dev, "base addres ioremap error!\n");
		ret = PTR_ERR(slave->base);
		goto free_slave;
	}

	slave->cs_polarity = cs_polarity;
	slave->phys_addr = cp_res.start;
	slave->reg_offset = regs_offset;
	slave->cs_sensitive = cs_sensitive;
	slave->pin_dir = pin_dir;
	slave->irq = irq;
	slave->pol = pol;
	slave->pha = pha;
	slave->mode = SPI_SLAVE_MODE;
	slave->buf_depth = SPI_SLAVE_BUF_DEPTH;
	slave->bytes_per_load = SPI_SLAVE_COPY_LENGTH;
	slave->bits_per_word = SPI_SLAVE_BITS_PER_WORD;

	slave->enable = mcspi_slave_enable_transfer;
	slave->disable = mcspi_slave_disable;
	slave->set_transfer = mcspi_slave_setup_pio_transfer;
	slave->clr_transfer = mcspi_slave_clr_pio_transfer;

	platform_set_drvdata(pdev, slave);

	pm_runtime_use_autosuspend(&pdev->dev);
	pm_runtime_set_autosuspend_delay(&pdev->dev, SPI_AUTOSUSPEND_TIMEOUT);
	pm_runtime_enable(&pdev->dev);

	ret = pm_runtime_get_sync(&pdev->dev);

	if (ret < 0)
		goto disable_pm;

	ret = mcspi_slave_setup(slave);
	if (ret < 0)
		goto disable_pm;

	ret = devm_spislave_register_slave(&pdev->dev, slave);
	if (ret) {
		dev_dbg(&slave->dev, "register device error\n");
		goto disable_pm;
	}

	return ret;

disable_pm:
	pm_runtime_dont_use_autosuspend(&pdev->dev);
	pm_runtime_put_sync(&pdev->dev);
	pm_runtime_disable(&pdev->dev);

free_slave:
	if (slave != NULL) {
		put_device(&slave->dev);
		mcspi_slave_clean_up(slave);
	}

	return ret;
}

static int mcspi_slave_remove(struct platform_device *pdev)
{
	struct spi_slave *slave;

	slave = platform_get_drvdata(pdev);

	mcspi_slave_clean_up(slave);

	pm_runtime_dont_use_autosuspend(&pdev->dev);
	pm_runtime_put_sync(&pdev->dev);
	pm_runtime_disable(&pdev->dev);

	return 0;
}

static struct platform_driver mcspi_slave_driver = {
	.probe	= mcspi_slave_probe,
	.remove = mcspi_slave_remove,
	.driver = {
		.name =	DRIVER_NAME,
		.of_match_table = of_match_ptr(mcspi_slave_of_match),
	},
};

module_platform_driver(mcspi_slave_driver);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Patryk Mezydlo, <mezydlo.p@gmail.com>");
MODULE_DESCRIPTION("SPI slave for McSPI controller.");
MODULE_VERSION("1.0");
