// SPDX-License-Identifier: GPL-2.0-only
/*
 * Bounded pre-capture health check and recovery for the experimental
 * bridge_link source path. Board initialization and receiver initialization
 * remain owned by the parent; this helper only re-runs the existing upstream
 * acquisition and TX1 native enable closures when a link is not healthy.
 */
#include <linux/bitops.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/types.h>

#include "gc573_bridge_acquire.h"
#include "gc573_bridge_recover.h"
#include "gc573_bridge_tx.h"
#include "gc573_pure.h"

#define GC573_RECOVER_RX_ADDRESS       0x70
#define GC573_RECOVER_TX1_ADDRESS      0x6a
#define GC573_RECOVER_RX_BANK          0x0f
#define GC573_RECOVER_RX_STATUS13      0x13
#define GC573_RECOVER_RX_STATUS14      0x14
#define GC573_RECOVER_RX_STATUS19      0x19
#define GC573_RECOVER_TX_STATUS03      0x03

#define GC573_RECOVER_RX_MATURE_MASK   (BIT(7) | BIT(4) | BIT(3) | BIT(0))
#define GC573_RECOVER_RX_MATURE_VALUE  GC573_RECOVER_RX_MATURE_MASK
#define GC573_RECOVER_RX_LANES_MASK    GENMASK(5, 3)
#define GC573_RECOVER_RX_LANES_VALUE   GC573_RECOVER_RX_LANES_MASK
#define GC573_RECOVER_RX_SCDT          BIT(7)
#define GC573_RECOVER_TX_LOCK_MASK     0x0f
#define GC573_RECOVER_TX_LOCK_VALUE    0x0f

static int gc573_recover_select_rx_bank0(struct gc573_device *dev)
{
	u8 bank;
	int ret;

	ret = gc573_i2c_read_reg(dev, GC573_RECOVER_RX_ADDRESS,
				GC573_RECOVER_RX_BANK, &bank);
	if (ret)
		return ret;
	if (bank == 0xff)
		return -ENODEV;
	return gc573_i2c_write_reg(dev, GC573_RECOVER_RX_ADDRESS,
				   GC573_RECOVER_RX_BANK, bank & ~GENMASK(1, 0));
}

static int gc573_recover_read_link_status(struct gc573_device *dev,
					  u8 *status13, u8 *status14,
					  u8 *status19, u8 *tx_status)
{
	int ret;

	ret = gc573_recover_select_rx_bank0(dev);
	if (ret)
		return ret;
	ret = gc573_i2c_read_reg(dev, GC573_RECOVER_RX_ADDRESS,
				GC573_RECOVER_RX_STATUS13, status13);
	if (ret)
		return ret;
	ret = gc573_i2c_read_reg(dev, GC573_RECOVER_RX_ADDRESS,
				GC573_RECOVER_RX_STATUS14, status14);
	if (ret)
		return ret;
	ret = gc573_i2c_read_reg(dev, GC573_RECOVER_RX_ADDRESS,
				GC573_RECOVER_RX_STATUS19, status19);
	if (ret)
		return ret;
	return gc573_i2c_read_reg(dev, GC573_RECOVER_TX1_ADDRESS,
				 GC573_RECOVER_TX_STATUS03, tx_status);
}

static bool gc573_recover_rx_mature(u8 status13, u8 status14, u8 status19)
{
	return (status13 & GC573_RECOVER_RX_MATURE_MASK) ==
			GC573_RECOVER_RX_MATURE_VALUE &&
	       (status14 & GC573_RECOVER_RX_LANES_MASK) ==
			GC573_RECOVER_RX_LANES_VALUE &&
	       (status19 & GC573_RECOVER_RX_SCDT);
}

int gc573_bridge_check_capture(struct gc573_device *dev)
{
	bool receiver_locked = false;
	bool rx_locked;
	u8 status13 = 0xff, status14 = 0xff, status19 = 0xff;
	u8 tx_status = 0xff;
	int ret, restore_ret;

	if (!dev || !dev->it6805 || !dev->it6805->get_signal_status)
		return -EINVAL;

	ret = gc573_recover_read_link_status(dev, &status13, &status14,
					     &status19, &tx_status);
	/* RX 0x14 == 0xff occurs on the qualified healthy baseline. */
	if (!ret && (status13 == 0xff || status19 == 0xff ||
		     tx_status == 0xff))
		ret = -ENODEV;
	if (!ret) {
		rx_locked = gc573_recover_rx_mature(status13, status14,
						   status19);
		ret = dev->it6805->get_signal_status(dev, &receiver_locked);
		if (!ret && (!rx_locked ||
			     (tx_status & GC573_RECOVER_TX_LOCK_MASK) !=
					GC573_RECOVER_TX_LOCK_VALUE ||
			     !receiver_locked))
			ret = -ENOLINK;
	}

	/* Bank selection writes, so this is not strictly read-only. */
	restore_ret = gc573_recover_select_rx_bank0(dev);
	if (!ret && restore_ret)
		ret = restore_ret;
	if (ret && dev->pdev)
		dev_err(&dev->pdev->dev,
			"active capture link check failed: error=%d RX=%02x/%02x/%02x TX1=%02x IT6805-lock=%u\n",
			ret, status13, status14, status19, tx_status,
			receiver_locked);
	return ret;
}

int gc573_bridge_prepare_capture(struct gc573_device *dev)
{
	bool receiver_locked = false;
	bool rx_locked;
	u8 status13 = 0xff, status14 = 0xff, status19 = 0xff;
	u8 tx_status = 0xff;
	int ret, restore_ret;
	const char *action = "not ready";
	u32 old_width = dev ? dev->input_width : 0;
	u32 old_height = dev ? dev->input_height : 0;
	u32 old_htotal = dev ? dev->input_htotal : 0;
	u32 old_vtotal = dev ? dev->input_vtotal : 0;
	u32 old_vic = dev ? dev->input_vic : 0;
	bool old_rgb_limited = dev ? dev->input_rgb_limited : false;

	if (!dev || !dev->pdev || !dev->it6805 ||
	    !dev->it6805->get_signal_status)
		return -EINVAL;

	ret = gc573_recover_read_link_status(dev, &status13, &status14,
					     &status19, &tx_status);
	/* RX 0x14 == 0xff occurs on the qualified healthy baseline. */
	if (!ret && (status13 == 0xff || status19 == 0xff ||
		     tx_status == 0xff))
		ret = -ENODEV;
	if (ret)
		goto out;

	rx_locked = gc573_recover_rx_mature(status13, status14, status19);
	ret = dev->it6805->get_signal_status(dev, &receiver_locked);
	if (ret)
		goto out;

	if (rx_locked && (tx_status & GC573_RECOVER_TX_LOCK_MASK) ==
			 GC573_RECOVER_TX_LOCK_VALUE && receiver_locked) {
		/* Recheck live AVI/raster even when all lock bits were retained. */
		action = "native mode validation failed";
		ret = gc573_bridge_validate_native(dev);
		if (ret)
			goto out;
		if (dev->input_width == old_width && dev->input_height == old_height &&
		    dev->input_htotal == old_htotal && dev->input_vtotal == old_vtotal &&
		    dev->input_vic == old_vic &&
		    dev->input_rgb_limited == old_rgb_limited) {
			action = "already healthy";
			goto out;
		}
		/* A retained lock does not mean the TX clock profile is unchanged. */
		ret = gc573_bridge_tx_enable_native(dev);
		action = "mode reconfigured";
		goto out;
	}

	dev_info(&dev->pdev->dev,
		 "bridge_link pre-capture recovery required RX=%02x/%02x/%02x TX1=%02x IT6805-lock=%u\n",
		 status13, status14, status19, tx_status, receiver_locked);
	ret = gc573_bridge_acquire_start(dev);
	if (ret)
		goto out;
	ret = gc573_bridge_acquire_wait(dev);
	if (ret)
		goto out;
	ret = gc573_bridge_tx_enable_native(dev);
	if (ret)
		goto out;
	/* The TX setup validates mode internally; revalidate at the STREAMON edge. */
	action = "post-recovery native mode validation failed";
	ret = gc573_bridge_validate_native(dev);
	if (ret)
		goto out;
	action = "recovered";

out:
	/* The TX validator may inspect banked RX data; leave RX in bank 0. */
	restore_ret = gc573_recover_select_rx_bank0(dev);
	if (!ret && restore_ret)
		ret = restore_ret;
	if (ret) {
		/* Do not publish a new raster if its TX configuration failed. */
		dev->input_width = old_width;
		dev->input_height = old_height;
		dev->input_htotal = old_htotal;
		dev->input_vtotal = old_vtotal;
		dev->input_vic = old_vic;
		dev->input_rgb_limited = old_rgb_limited;
		dev_err(&dev->pdev->dev,
			"bridge_link pre-capture %s: error=%d RX=%02x/%02x/%02x TX1=%02x IT6805-lock=%u\n",
			action, ret, status13, status14, status19, tx_status,
			receiver_locked);
		return ret;
	}
	dev_info(&dev->pdev->dev, "bridge_link pre-capture %s\n", action);
	return 0;
}
