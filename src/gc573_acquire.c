// SPDX-License-Identifier: GPL-2.0-only
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/types.h>

#include "gc573_pure.h"
#include "gc573_acquire.h"

#define IT6805_ADDRESS              0x90
#define IT6805_REG_BANK             0x0f
#define IT6805_BANK_MASK            0x07
#define IT6805_BANK_0               0x00
#define IT6805_BANK_CAOF_PORT0      0x03

#define IT6805_IRQ_SYS05_MASK       0xff
#define IT6805_IRQ_SYS06_MASK       0xff
#define IT6805_IRQ_SYS08_MASK       0xff
/* The reference deliberately leaves SYS register 0x09 bit 2 unacknowledged. */
#define IT6805_IRQ_SYS09_MASK       0xfb
#define IT6805_IRQ_EQ_MASK          0xf7

static int it6805_read(struct gc573_device *dev, u8 reg, u8 *value)
{
	return gc573_i2c_read_reg(dev, IT6805_ADDRESS, reg, value);
}

static int it6805_write(struct gc573_device *dev, u8 reg, u8 value)
{
	return gc573_i2c_write_reg(dev, IT6805_ADDRESS, reg, value);
}

static int it6805_update_bits(struct gc573_device *dev, u8 reg,
			      u8 mask, u8 value)
{
	u8 old_value;
	int ret;

	ret = it6805_read(dev, reg, &old_value);
	if (ret)
		return ret;

	return it6805_write(dev, reg, (old_value & ~mask) | (value & mask));
}

static int it6805_set_bank(struct gc573_device *dev, u8 bank)
{
	return it6805_update_bits(dev, IT6805_REG_BANK, IT6805_BANK_MASK, bank);
}

/* Port-0 reset/start sequence mirrored from gc555-it6805-core.c. */
static int it6805_reset_eq_port0(struct gc573_device *dev)
{
	int ret;

	ret = it6805_set_bank(dev, IT6805_BANK_CAOF_PORT0);
	if (ret)
		return ret;
	ret = it6805_write(dev, 0x2c, 0x00);
	if (ret)
		return ret;
	ret = it6805_write(dev, 0x2d, 0x07);
	if (ret)
		return ret;
	ret = it6805_set_bank(dev, IT6805_BANK_0);
	if (ret)
		return ret;
	/* Pending EQ IRQs were already acknowledged with their precise W1C mask. */
	ret = it6805_write(dev, 0x23, 0xb0);
	if (ret)
		return ret;
	usleep_range(1000, 2000);
	ret = it6805_write(dev, 0x23, 0xa0);
	if (ret)
		return ret;
	ret = it6805_update_bits(dev, 0x23, BIT(1), BIT(1));
	if (ret)
		return ret;
	ret = it6805_set_bank(dev, IT6805_BANK_CAOF_PORT0);
	if (ret)
		return ret;
	ret = it6805_write(dev, 0x27, 0x9f);
	if (ret)
		return ret;
	ret = it6805_write(dev, 0x28, 0x9f);
	if (ret)
		return ret;
	ret = it6805_write(dev, 0x29, 0x9f);
	if (ret)
		return ret;
	ret = it6805_write(dev, 0x22, 0x00);
	if (ret)
		return ret;
	ret = it6805_update_bits(dev, 0x4b, BIT(7), 0);
	if (ret)
		return ret;
	return it6805_set_bank(dev, IT6805_BANK_0);
}

static int it6805_start_eq_port0(struct gc573_device *dev, u8 status14)
{
	int ret;

	ret = it6805_set_bank(dev, IT6805_BANK_CAOF_PORT0);
	if (ret)
		return ret;
	ret = it6805_update_bits(dev, 0xa7, BIT(6),
				 status14 & (BIT(6) | BIT(0)) ? BIT(6) : 0);
	if (ret)
		return ret;
	return it6805_set_bank(dev, IT6805_BANK_0);
}

static int it6805_restart_eq_port0(struct gc573_device *dev, u8 status14)
{
	int ret;

	ret = it6805_reset_eq_port0(dev);
	if (ret)
		return ret;

	return it6805_start_eq_port0(dev, status14);
}

/*
 * Port-0 branch of the shipped iTE6805_Trigger_EQ (ite6805_EQ.o). In this
 * initialization path the private handle's selected-port byte is zero, so
 * the object selects CAOF bank 3. The status-dependent EQ values and pulse
 * below follow that branch's disassembled writes directly.
 */
int gc573_it6805_trigger_eq(struct gc573_device *dev)
{
	u8 status14;
	u8 eq20, eq21;
	int ret;

	if (!dev)
		return -EINVAL;

	ret = it6805_set_bank(dev, IT6805_BANK_0);
	if (ret)
		goto bank0;
	ret = it6805_write(dev, 0x07, 0xff);
	if (ret)
		goto bank0;
	ret = it6805_write(dev, 0x23, 0xb0);
	if (ret)
		goto bank0;
	msleep(10);
	ret = it6805_write(dev, 0x23, 0xa0);
	if (ret)
		goto bank0;
	ret = it6805_read(dev, 0x14, &status14);
	if (ret)
		goto bank0;

	/* Trigger_EQ uses status14 bit 0 to choose its fast EQ tuple. */
	eq20 = status14 & BIT(0) ? 0x36 : 0x1b;
	eq21 = status14 & BIT(0) ? 0x0e : 0x03;
	ret = it6805_set_bank(dev, IT6805_BANK_CAOF_PORT0);
	if (ret)
		goto bank0;
	ret = it6805_write(dev, 0x20, eq20);
	if (ret)
		goto bank0;
	ret = it6805_write(dev, 0x21, eq21);
	if (ret)
		goto bank0;
	ret = it6805_write(dev, 0x26, 0x00);
	if (ret)
		goto bank0;
	ret = it6805_write(dev, 0x27, 0x00);
	if (ret)
		goto bank0;
	ret = it6805_write(dev, 0x28, 0x00);
	if (ret)
		goto bank0;
	ret = it6805_write(dev, 0x29, 0x00);
	if (ret)
		goto bank0;
	ret = it6805_write(dev, 0x22, 0x38);
	if (ret)
		goto bank0;
	ret = it6805_update_bits(dev, 0x22, BIT(2), BIT(2));
	if (ret)
		goto bank0;
	msleep(1);
	ret = it6805_update_bits(dev, 0x22, BIT(2), 0);

bank0:
	{
		int cleanup_ret = it6805_set_bank(dev, IT6805_BANK_0);

		if (!ret)
			ret = cleanup_ret;
	}
	return ret;
}

/*
 * This services only acquisition-related port-0 IRQ sources. The full EQ_fsm
 * also checks clock validity and HDMI2 mode and selects an HDMI-version EQ
 * table; that progression is outside this bounded IRQ service. The separately
 * callable trigger above is the decoded port-0 Trigger_EQ branch for the
 * receiver initialization path that explicitly selects port 0.
 */
int gc573_it6805_service_acquisition(struct gc573_device *dev)
{
	u8 irq05, irq06, irq07, irq08, irq09;
	u8 status13, status14;
	bool restart_eq;
	int ret;

	if (!dev)
		return -EINVAL;

	ret = it6805_set_bank(dev, IT6805_BANK_0);
	if (ret)
		return ret;
	ret = it6805_read(dev, 0x05, &irq05);
	if (ret)
		goto bank0;
	ret = it6805_read(dev, 0x06, &irq06);
	if (ret)
		goto bank0;
	ret = it6805_read(dev, 0x07, &irq07);
	if (ret)
		goto bank0;
	ret = it6805_read(dev, 0x08, &irq08);
	if (ret)
		goto bank0;
	ret = it6805_read(dev, 0x09, &irq09);
	if (ret)
		goto bank0;
	ret = it6805_read(dev, 0x13, &status13);
	if (ret)
		goto bank0;
	ret = it6805_read(dev, 0x14, &status14);
	if (ret)
		goto bank0;

	/* W1C only the observed valid bits, matching the local reference handler. */
	if (irq05) {
		ret = it6805_write(dev, 0x05, irq05 & IT6805_IRQ_SYS05_MASK);
		if (ret)
			goto bank0;
	}
	if (irq06) {
		ret = it6805_write(dev, 0x06, irq06 & IT6805_IRQ_SYS06_MASK);
		if (ret)
			goto bank0;
	}
	if (irq08) {
		ret = it6805_write(dev, 0x08, irq08 & IT6805_IRQ_SYS08_MASK);
		if (ret)
			goto bank0;
	}
	if (irq09 & IT6805_IRQ_SYS09_MASK) {
		ret = it6805_write(dev, 0x09, irq09 & IT6805_IRQ_SYS09_MASK);
		if (ret)
			goto bank0;
	}
	if (irq07 & IT6805_IRQ_EQ_MASK) {
		ret = it6805_write(dev, 0x07, irq07 & IT6805_IRQ_EQ_MASK);
		if (ret)
			goto bank0;
	}

	/*
	 * SYS05 bit 2 is a clock-change reset request unless EQ is already locked;
	 * SYS06 bit 0 requests it when the EQ status field says acquisition failed.
	 * SYS08 bit 0 is the reference's lost-ratio event and always restarts EQ.
	 */
	restart_eq = ((irq05 & BIT(2)) && !(status13 & BIT(4))) ||
		     ((irq06 & BIT(0)) && (status14 & GENMASK(5, 3))) ||
		     (irq08 & BIT(0));
	if (restart_eq) {
		ret = it6805_restart_eq_port0(dev, status14);
		if (ret)
			goto bank0;
	}

	/* The reference maps SYS08 detector IRQ bit 7 to bank-0 register 0x4c. */
	if (irq08 & BIT(7)) {
		ret = it6805_set_bank(dev, IT6805_BANK_0);
		if (ret)
			goto bank0;
		ret = it6805_update_bits(dev, 0x4c, BIT(7), BIT(7));
		if (ret)
			goto bank0;
	}

	/*
	 * EQ IRQ bits 7..4 are sticky event indications. The reference clears
	 * CAOF-port0 register 0x22 bit 2 when any such event was captured; copying
	 * the vendor's table/state-machine advancement requires the missing FSM.
	 */
	if (irq07 & GENMASK(7, 4)) {
		ret = it6805_set_bank(dev, IT6805_BANK_CAOF_PORT0);
		if (ret)
			goto bank0;
		ret = it6805_update_bits(dev, 0x22, BIT(2), 0);
		if (ret)
			goto bank0;
	}
	ret = 0;

bank0:
	/* Keep subsequent receiver operations on the documented default bank. */
	{
		int cleanup_ret = it6805_set_bank(dev, IT6805_BANK_0);

		if (!ret)
			ret = cleanup_ret;
	}
	return ret;
}
