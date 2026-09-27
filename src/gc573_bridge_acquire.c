// SPDX-License-Identifier: GPL-2.0-only
/*
 * Bounded IT6664 upstream RX acquisition.
 *
 * Register sequences and status predicates are derived from
 * gc555-it6664-core.c: it6664_prepare_initial_rx_hpd(),
 * gc555_it6664_rx_set_hpd(), it6664_handle_rx_detect_bus(),
 * it6664_handle_rx_signal_irq(), it6664_start_rx_eq14(), and
 * it6664_handle_rx_scdt_lock(). HDMI 2 EQ results use the bounded EQ helper.
 */
#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/types.h>

#include "gc573_pure.h"
#include "gc573_bridge_acquire.h"
#include "gc573_bridge_eq.h"

#define GC573_IT6664_SWITCH_ADDR       0x58
#define GC573_IT6664_RX_ADDR           0x70

#define GC573_RX_REG_BANK              0x0f
#define GC573_RX_REG_STATUS            0x13
#define GC573_RX_REG_STATUS_14         0x14
#define GC573_RX_REG_SCDT_STATUS       0x19
#define GC573_RX_BANK_MASK             GENMASK(1, 0)
#define GC573_RX_BANK_0                0x00
#define GC573_RX_BANK_3                0x03

#define GC573_SWITCH_REG_0C            0x0c
#define GC573_SWITCH_REG_10            0x10
#define GC573_SWITCH_REG_IRQ_STATUS    0x05

#define GC573_ACQUIRE_TIMEOUT_MS       10000
#define GC573_ACQUIRE_POLL_MS          20
#define GC573_HPD_LOW_MS               500
#define GC573_HPD_TEST_LOW_MS          1500

static int gc573_acquire_read(struct gc573_device *dev, u8 address,
			      u8 reg, u8 *value)
{
	return gc573_i2c_read_reg(dev, address, reg, value);
}

static int gc573_acquire_write(struct gc573_device *dev, u8 address,
			       u8 reg, u8 value)
{
	return gc573_i2c_write_reg(dev, address, reg, value);
}

static int gc573_acquire_update(struct gc573_device *dev, u8 address,
				u8 reg, u8 mask, u8 value)
{
	u8 old_value;
	int ret;

	ret = gc573_acquire_read(dev, address, reg, &old_value);
	if (ret)
		return ret;

	return gc573_acquire_write(dev, address, reg,
				   (old_value & ~mask) | (value & mask));
}

static int gc573_acquire_select_rx_bank(struct gc573_device *dev, u8 bank)
{
	return gc573_acquire_update(dev, GC573_IT6664_RX_ADDR,
				    GC573_RX_REG_BANK, GC573_RX_BANK_MASK,
				    bank);
}

/* Reference's initial bank-3 HPD preparation writes. */
static int gc573_acquire_prepare_hpd(struct gc573_device *dev)
{
	int ret;
	int cleanup_ret;

	ret = gc573_acquire_select_rx_bank(dev, GC573_RX_BANK_3);
	if (!ret)
		ret = gc573_acquire_write(dev, GC573_IT6664_RX_ADDR, 0xab,
					  0x4a);
	if (!ret)
		ret = gc573_acquire_write(dev, GC573_IT6664_RX_ADDR, 0xac,
					  0x40);
	cleanup_ret = gc573_acquire_select_rx_bank(dev, GC573_RX_BANK_0);
	return ret ? ret : cleanup_ret;
}

/* Match gc555_it6664_rx_set_hpd() for ordinary HDMI bus_mode 0. */
static int gc573_acquire_set_hpd(struct gc573_device *dev, bool high)
{
	u8 status;
	u8 value;
	int ret;
	int cleanup_ret;

	if (high) {
		ret = gc573_acquire_read(dev, GC573_IT6664_RX_ADDR,
					 GC573_RX_REG_STATUS, &status);
		if (ret)
			return ret;
		if (status == 0xff)
			return -ENODEV;
		/* The reference defers HPD assertion until detect-bus bit 0. */
		if (!(status & BIT(0)))
			return 0;
	}

	ret = gc573_acquire_select_rx_bank(dev, GC573_RX_BANK_3);
	if (ret)
		goto cleanup;
	ret = gc573_acquire_read(dev, GC573_IT6664_RX_ADDR, 0xab, &value);
	if (ret)
		goto cleanup;
	if (value == 0xff) {
		ret = -ENODEV;
		goto cleanup;
	}
	if (high) {
		if (value != 0xca)
			ret = gc573_acquire_write(dev, GC573_IT6664_RX_ADDR,
						  0xab, 0xca);
	} else if (value == 0xca) {
		ret = gc573_acquire_write(dev, GC573_IT6664_RX_ADDR, 0xab,
					  0x4a);
		if (!ret)
			ret = gc573_acquire_write(dev, GC573_IT6664_RX_ADDR,
						  0xab, 0x00);
		if (!ret)
			ret = gc573_acquire_write(dev, GC573_IT6664_RX_ADDR,
						  0xac, 0x00);
	}

cleanup:
	cleanup_ret = gc573_acquire_select_rx_bank(dev, GC573_RX_BANK_0);
	if (!ret)
		ret = cleanup_ret;
	if (ret)
		return ret;

	ret = gc573_acquire_write(dev, GC573_IT6664_RX_ADDR, 0x26,
				  high ? 0x00 : 0xff);
	if (ret)
		return ret;
	return gc573_acquire_write(dev, GC573_IT6664_RX_ADDR, 0x55,
				   high ? 0xff : 0x00);
}

int gc573_bridge_acquire_start(struct gc573_device *dev)
{
	u8 status;
	int ret;
	int cleanup_ret;

	if (!dev)
		return -EINVAL;
	ret = gc573_acquire_select_rx_bank(dev, GC573_RX_BANK_0);
	if (ret)
		return ret;
	ret = gc573_acquire_read(dev, GC573_IT6664_RX_ADDR,
				 GC573_RX_REG_STATUS, &status);
	if (ret)
		return ret;
	if (status == 0xff)
		return -ENODEV;
	/* Same mode extraction as it6664_handle_rx_signal_start(). */
	if (((status >> 5) & 0x02) != 0)
		return -EOPNOTSUPP;

	ret = gc573_acquire_prepare_hpd(dev);
	if (ret)
		goto bank_zero;
	ret = gc573_acquire_set_hpd(dev, false);
	if (ret)
		goto bank_zero;
	msleep(GC573_HPD_LOW_MS);
	ret = gc573_acquire_set_hpd(dev, true);

bank_zero:
	cleanup_ret = gc573_acquire_select_rx_bank(dev, GC573_RX_BANK_0);
	return ret ? ret : cleanup_ret;
}

/*
 * Diagnostic HPD cycle for an already acquired HDMI input. The caller owns
 * bridge-bank serialization; an active DMA capture is permitted.
 */
int gc573_bridge_test_hpd_cycle(struct gc573_device *dev)
{
	u8 status;
	u8 hpd;
	int ret;
	int bank_ret;
	int high_ret;
	int cleanup_ret;

	if (!dev)
		return -EINVAL;

	ret = gc573_acquire_select_rx_bank(dev, GC573_RX_BANK_0);
	if (ret)
		goto bank_zero;
	ret = gc573_acquire_read(dev, GC573_IT6664_RX_ADDR,
				 GC573_RX_REG_STATUS, &status);
	if (ret)
		goto bank_zero;
	if (status == 0xff) {
		ret = -ENODEV;
		goto bank_zero;
	}
	/* Ordinary HDMI mode with the upstream detect/bus-5V bit asserted. */
	if (((status >> 5) & 0x02) != 0) {
		ret = -EOPNOTSUPP;
		goto bank_zero;
	}
	if (!(status & BIT(0))) {
		ret = -EAGAIN;
		goto bank_zero;
	}

	ret = gc573_acquire_select_rx_bank(dev, GC573_RX_BANK_3);
	if (ret)
		goto bank_zero;
	ret = gc573_acquire_read(dev, GC573_IT6664_RX_ADDR, 0xab, &hpd);
	if (ret)
		goto bank_zero;
	if (hpd == 0xff) {
		ret = -ENODEV;
		goto bank_zero;
	}
	if (hpd != 0xca) {
		ret = -EAGAIN;
		goto bank_zero;
	}

	ret = gc573_acquire_set_hpd(dev, false);
	if (!ret)
		msleep(GC573_HPD_TEST_LOW_MS);
	{
		bank_ret = gc573_acquire_select_rx_bank(dev, GC573_RX_BANK_0);
		high_ret = bank_ret ? bank_ret : gc573_acquire_set_hpd(dev, true);
		if (!ret)
			ret = bank_ret ? bank_ret : high_ret;
		if (!ret) {
			ret = gc573_acquire_read(dev, GC573_IT6664_RX_ADDR,
						 GC573_RX_REG_STATUS, &status);
			if (!ret && status == 0xff)
				ret = -ENODEV;
			else if (!ret &&
				 (((status >> 5) & 0x02) || !(status & BIT(0))))
				ret = -EAGAIN;
		}
		if (!ret) {
			ret = gc573_acquire_select_rx_bank(dev, GC573_RX_BANK_3);
			if (!ret)
				ret = gc573_acquire_read(dev, GC573_IT6664_RX_ADDR,
							 0xab, &hpd);
			if (!ret && hpd == 0xff)
				ret = -ENODEV;
			else if (!ret && hpd != 0xca)
				ret = -EAGAIN;
		}
	}

bank_zero:
	cleanup_ret = gc573_acquire_select_rx_bank(dev, GC573_RX_BANK_0);
	return ret ? ret : cleanup_ret;
}

/*
 * Exact detect-bus handler closure from it6664_handle_rx_detect_bus().
 * The supplied event snapshot is unnecessary here: wait() reads the same
 * live status predicate before calling this closure.
 */
static int gc573_acquire_detect_bus(struct gc573_device *dev)
{
	u8 status;
	bool pulse_active = false;
	bool rx_bank_three = false;
	int ret;
	int cleanup_ret;

	ret = gc573_acquire_update(dev, GC573_IT6664_SWITCH_ADDR,
				   GC573_SWITCH_REG_0C, BIT(2), BIT(2));
	if (ret)
		return ret;
	pulse_active = true;
	ret = gc573_acquire_update(dev, GC573_IT6664_SWITCH_ADDR,
				   GC573_SWITCH_REG_10, BIT(6), 0);
	if (ret)
		goto cleanup;
	ret = gc573_acquire_update(dev, GC573_IT6664_SWITCH_ADDR,
				   GC573_SWITCH_REG_10, BIT(6), BIT(6));
	if (ret)
		goto cleanup;
	ret = gc573_acquire_update(dev, GC573_IT6664_SWITCH_ADDR,
				   GC573_SWITCH_REG_0C, BIT(2), 0);
	if (ret)
		goto cleanup;
	pulse_active = false;

	ret = gc573_acquire_select_rx_bank(dev, GC573_RX_BANK_0);
	if (ret)
		goto cleanup;
	ret = gc573_acquire_read(dev, GC573_IT6664_RX_ADDR,
				 GC573_RX_REG_STATUS, &status);
	if (ret)
		goto cleanup;
	if (status == 0xff) {
		ret = -ENODEV;
		goto cleanup;
	}
	if ((status & (BIT(6) | BIT(0))) != BIT(0)) {
		ret = -EAGAIN;
		goto cleanup;
	}

	ret = gc573_acquire_select_rx_bank(dev, GC573_RX_BANK_3);
	if (ret)
		goto cleanup;
	rx_bank_three = true;
	ret = gc573_acquire_update(dev, GC573_IT6664_RX_ADDR, 0x3a,
				   GENMASK(2, 1), BIT(1));
	if (ret)
		goto cleanup;
	ret = gc573_acquire_select_rx_bank(dev, GC573_RX_BANK_0);
	if (ret)
		goto cleanup;
	rx_bank_three = false;
	ret = gc573_acquire_update(dev, GC573_IT6664_RX_ADDR, 0x29,
				   BIT(0), 0);
	if (ret)
		goto cleanup;
	ret = gc573_acquire_update(dev, GC573_IT6664_RX_ADDR, 0x26,
				   GENMASK(3, 2), 0);
	if (ret)
		goto cleanup;
	ret = gc573_acquire_update(dev, GC573_IT6664_SWITCH_ADDR, 0x69,
				   BIT(5) | GENMASK(3, 0), 0);
	if (ret)
		goto cleanup;
	ret = gc573_acquire_set_hpd(dev, true);

cleanup:
	if (rx_bank_three || ret) {
		cleanup_ret = gc573_acquire_select_rx_bank(dev,
							  GC573_RX_BANK_0);
		if (!ret)
			ret = cleanup_ret;
	}
	if (pulse_active) {
		cleanup_ret = gc573_acquire_update(dev,
						   GC573_IT6664_SWITCH_ADDR,
						   GC573_SWITCH_REG_0C,
						   BIT(2), 0);
		if (!ret)
			ret = cleanup_ret;
	}
	return ret;
}

/* Port of the RX-local part of it6664_handle_rx_signal_irq(). */
static int gc573_acquire_signal_start(struct gc573_device *dev)
{
	u8 status;
	int ret;

	ret = gc573_acquire_write(dev, GC573_IT6664_RX_ADDR, 0x05, BIT(2));
	if (ret)
		return ret;
	ret = gc573_acquire_update(dev, GC573_IT6664_RX_ADDR, 0x53,
				   0xe0, 0);
	if (ret)
		return ret;
	ret = gc573_acquire_update(dev, GC573_IT6664_RX_ADDR, 0x54,
				   0xff, 0);
	if (ret)
		return ret;
	ret = gc573_acquire_update(dev, GC573_IT6664_RX_ADDR, 0x55,
				   0x07, 0);
	if (ret)
		return ret;
	ret = gc573_acquire_write(dev, GC573_IT6664_RX_ADDR, 0x05, 0xe8);
	if (ret)
		return ret;
	ret = gc573_acquire_write(dev, GC573_IT6664_RX_ADDR, 0x06, 0xfe);
	if (ret)
		return ret;
	ret = gc573_acquire_read(dev, GC573_IT6664_RX_ADDR,
				 GC573_RX_REG_STATUS, &status);
	if (ret)
		return ret;
	if (status == 0xff)
		return -ENODEV;
	if (!(status & BIT(3)))
		return -EAGAIN;
	ret = gc573_acquire_update(dev, GC573_IT6664_RX_ADDR, 0x23,
				   BIT(1), 0);
	if (ret)
		return ret;
	return gc573_acquire_update(dev, GC573_IT6664_RX_ADDR, 0x54,
				    BIT(0), BIT(0));
}

/* The source's RX EQ start arm, followed by the source-selected EQ trigger. */
static int gc573_acquire_eq_start(struct gc573_device *dev, u8 status14)
{
	int ret;

	ret = gc573_acquire_update(dev, GC573_IT6664_RX_ADDR, 0x53,
				   0xe0, 0xe0);
	if (ret)
		return ret;
	ret = gc573_acquire_update(dev, GC573_IT6664_RX_ADDR, 0x55,
				   0x07, 0x07);
	if (ret)
		return ret;
	ret = gc573_acquire_update(dev, GC573_IT6664_RX_ADDR, 0x57,
				   0x0f, 0x0f);
	if (ret)
		return ret;
	ret = gc573_acquire_update(dev, GC573_IT6664_RX_ADDR, 0x5d,
				   0x06, 0x06);
	if (ret)
		return ret;
	ret = gc573_acquire_update(dev, GC573_IT6664_RX_ADDR, 0x5e,
				   BIT(3), BIT(3));
	if (ret)
		return ret;
	ret = gc573_acquire_update(dev, GC573_IT6664_RX_ADDR, 0x5f,
				   BIT(0), BIT(0));
	if (ret)
		return ret;

	if (status14 & BIT(6))
		return gc573_bridge_eq20_run(dev, status14);

	/* Source-derived EQ14 trigger sequence. */
	ret = gc573_acquire_write(dev, GC573_IT6664_RX_ADDR, 0x07, 0xff);
	if (ret)
		return ret;
	ret = gc573_acquire_write(dev, GC573_IT6664_RX_ADDR, 0x23, 0xb0);
	if (ret)
		return ret;
	ret = gc573_acquire_write(dev, GC573_IT6664_RX_ADDR, 0x23, 0xa0);
	if (ret)
		return ret;
	ret = gc573_acquire_select_rx_bank(dev, GC573_RX_BANK_3);
	if (ret)
		goto bank_zero;
	ret = gc573_acquire_write(dev, GC573_IT6664_RX_ADDR, 0x2c, 0x00);
	if (!ret)
		ret = gc573_acquire_write(dev, GC573_IT6664_RX_ADDR, 0x2d, 0x00);
	if (!ret)
		ret = gc573_acquire_write(dev, GC573_IT6664_RX_ADDR, 0x20, 0x36);
	if (!ret)
		ret = gc573_acquire_write(dev, GC573_IT6664_RX_ADDR, 0x21, 0x0e);
	if (!ret)
		ret = gc573_acquire_write(dev, GC573_IT6664_RX_ADDR, 0x26, 0x00);
	if (!ret)
		ret = gc573_acquire_write(dev, GC573_IT6664_RX_ADDR, 0x27, 0x1f);
	if (!ret)
		ret = gc573_acquire_write(dev, GC573_IT6664_RX_ADDR, 0x28, 0x1f);
	if (!ret)
		ret = gc573_acquire_write(dev, GC573_IT6664_RX_ADDR, 0x29, 0x1f);
	if (!ret)
		ret = gc573_acquire_update(dev, GC573_IT6664_RX_ADDR, 0x22,
					   GENMASK(5, 3), GENMASK(5, 3));
	if (!ret)
		ret = gc573_acquire_update(dev, GC573_IT6664_RX_ADDR, 0x22,
					   BIT(2), BIT(2));
	if (!ret) {
		usleep_range(1000, 2000);
		ret = gc573_acquire_update(dev, GC573_IT6664_RX_ADDR, 0x22,
					   BIT(2), 0);
	}

bank_zero:
	{
		int cleanup_ret = gc573_acquire_select_rx_bank(dev,
							      GC573_RX_BANK_0);

		return ret ? ret : cleanup_ret;
	}
}

/* Shared-switch/RX clock setup from it6664_handle_rx_scdt_lock(). */
static int gc573_acquire_configure_locked_clock(struct gc573_device *dev,
						       u8 status14)
{
	int ret;
	int cleanup_ret;

	ret = gc573_acquire_update(dev, GC573_IT6664_RX_ADDR, 0x40,
				   GENMASK(1, 0), 0);
	if (ret)
		return ret;
	ret = gc573_acquire_write(dev, GC573_IT6664_SWITCH_ADDR, 0x0b, 0xff);
	if (ret)
		return ret;
	ret = gc573_acquire_write(dev, GC573_IT6664_SWITCH_ADDR, 0x0b, 0x00);
	if (ret)
		return ret;
	ret = gc573_acquire_update(dev, GC573_IT6664_SWITCH_ADDR, 0x4e,
				   GENMASK(3, 0), GENMASK(3, 0));
	if (ret)
		return ret;
	ret = gc573_acquire_update(dev, GC573_IT6664_SWITCH_ADDR,
				   GC573_SWITCH_REG_0C, BIT(3), BIT(3));
	if (ret)
		return ret;
	ret = gc573_acquire_update(dev, GC573_IT6664_SWITCH_ADDR,
				   GC573_SWITCH_REG_0C, BIT(3), 0);
	if (ret)
		return ret;
	ret = gc573_acquire_update(dev, GC573_IT6664_SWITCH_ADDR, 0x4e,
				   GENMASK(3, 0), 0);
	if (ret)
		return ret;
	ret = gc573_acquire_update(dev, GC573_IT6664_SWITCH_ADDR, 0x67,
				   GENMASK(3, 0), 0);
	if (ret)
		return ret;
	ret = gc573_acquire_write(dev, GC573_IT6664_SWITCH_ADDR, 0x68, 0x00);
	if (ret)
		return ret;
	ret = gc573_acquire_select_rx_bank(dev, GC573_RX_BANK_3);
	if (ret)
		goto restore_bank;
	ret = gc573_acquire_update(dev, GC573_IT6664_RX_ADDR, 0xa7, BIT(6),
				   (status14 & BIT(0)) << 6);

restore_bank:
	cleanup_ret = gc573_acquire_select_rx_bank(dev, GC573_RX_BANK_0);
	return ret ? ret : cleanup_ret;
}

static int gc573_acquire_read_lock_status(struct gc573_device *dev,
					  u8 *status13, u8 *status14,
					  u8 *status19)
{
	int ret;

	ret = gc573_acquire_select_rx_bank(dev, GC573_RX_BANK_0);
	if (ret)
		return ret;
	ret = gc573_acquire_read(dev, GC573_IT6664_RX_ADDR,
				 GC573_RX_REG_STATUS, status13);
	if (ret)
		return ret;
	ret = gc573_acquire_read(dev, GC573_IT6664_RX_ADDR,
				 GC573_RX_REG_STATUS_14, status14);
	if (ret)
		return ret;
	return gc573_acquire_read(dev, GC573_IT6664_RX_ADDR,
				  GC573_RX_REG_SCDT_STATUS, status19);
}

/* Read, but do not acknowledge, the event registers for failure reporting. */
static void gc573_acquire_log_failure(struct gc573_device *dev, int cause)
{
	u8 status13 = 0xff;
	u8 status14 = 0xff;
	u8 status19 = 0xff;
	static const u8 irq_regs[] = {0x05, 0x06, 0x07, 0x08, 0x09, 0x10, 0x11, 0x12};
	u8 irq[8];
	u8 switch_irq = 0xff;
	unsigned int i;
	int ret;

	if (!dev->pdev)
		return;
	memset(irq, 0xff, sizeof(irq));
	ret = gc573_acquire_read_lock_status(dev, &status13, &status14,
					     &status19);
	if (ret)
		dev_err(&dev->pdev->dev,
			"IT6664 RX acquisition failure=%d status read failed=%d\n",
			cause, ret);
	for (i = 0; i < ARRAY_SIZE(irq); i++) {
		ret = gc573_acquire_read(dev, GC573_IT6664_RX_ADDR, irq_regs[i],
					 &irq[i]);
		if (ret)
			break;
	}
	ret = gc573_acquire_read(dev, GC573_IT6664_SWITCH_ADDR,
				 GC573_SWITCH_REG_IRQ_STATUS, &switch_irq);
	if (ret)
		switch_irq = 0xff;
	dev_err(&dev->pdev->dev,
		"IT6664 RX acquisition failed=%d status13=%02x status14=%02x status19=%02x irq05/06/07/08/09/10/11/12=%02x/%02x/%02x/%02x/%02x/%02x/%02x/%02x switch05=%02x\n",
		cause, status13, status14, status19,
		irq[0], irq[1], irq[2], irq[3],
		irq[4], irq[5], irq[6], irq[7], switch_irq);
}

int gc573_bridge_acquire_wait(struct gc573_device *dev)
{
	unsigned long deadline;
	bool detect_bus_done = false;
	bool signal_started = false;
	u8 status13;
	u8 status14;
	u8 status19;
	int ret;

	if (!dev)
		return -EINVAL;
	deadline = jiffies + msecs_to_jiffies(GC573_ACQUIRE_TIMEOUT_MS);
	for (;;) {
		ret = gc573_acquire_read_lock_status(dev, &status13,
						     &status14, &status19);
		if (ret) {
			gc573_acquire_log_failure(dev, ret);
			return ret;
		}
		if (status13 == 0xff || status19 == 0xff) {
			gc573_acquire_log_failure(dev, -ENODEV);
			return -ENODEV;
		}
		/* Source-derived detect-bus level test; no inferred status bits. */
		if (!detect_bus_done &&
		    (status13 & (BIT(6) | BIT(0))) == BIT(0)) {
			ret = gc573_acquire_detect_bus(dev);
			if (ret && ret != -EAGAIN) {
				gc573_acquire_log_failure(dev, ret);
				return ret;
			}
			if (!ret)
				detect_bus_done = true;
		}
		/*
		 * These exact stable-level bits gate the reference's SCDT-lock
		 * handling: mature RX status, all three lane-ready bits, and SCDT.
		 */
		if (detect_bus_done &&
		    (status13 & (BIT(7) | BIT(4) | BIT(3) | BIT(0))) ==
			(BIT(7) | BIT(4) | BIT(3) | BIT(0)) &&
		    (status14 & GENMASK(5, 3)) == GENMASK(5, 3) &&
		    (status19 & BIT(7))) {
			ret = gc573_acquire_configure_locked_clock(dev, status14);
			if (ret) {
				gc573_acquire_log_failure(dev, ret);
				return ret;
			}
			return 0;
		}
		if (detect_bus_done && !signal_started && (status13 & BIT(3))) {
			ret = gc573_acquire_signal_start(dev);
			if (ret && ret != -EAGAIN) {
				gc573_acquire_log_failure(dev, ret);
				return ret;
			}
			if (!ret) {
				ret = gc573_acquire_eq_start(dev, status14);
				if (ret) {
					gc573_acquire_log_failure(dev, ret);
					return ret;
				}
				signal_started = true;
			}
		}
		if (time_after_eq(jiffies, deadline)) {
			gc573_acquire_log_failure(dev, -ETIMEDOUT);
			return -ETIMEDOUT;
		}
		msleep(GC573_ACQUIRE_POLL_MS);
	}
}
