// SPDX-License-Identifier: GPL-2.0-only
/*
 * Bounded IT6664 switch and reference-clock bootstrap.
 *
 * Sequence and SIPROM/RCLK logic adapted from the GPL-2.0-only
 * gc555-it6664-core.c reference, functions it6664_apply_pre_rclk_sequence,
 * it6664_read_siprom, and it6664_calibrate_rclk (source lines 355-596).
 * Board-specific identities and even 8-bit I2C addresses are validated
 * against evidence/bridge-baseline-20260924.txt.
 */
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/types.h>

#include "gc573_bridge_clock.h"
#include "gc573_pure.h"

#define GC573_IT6664_SWITCH_ADDR		0x58
#define GC573_IT6664_TX_COMMON_ADDR		0x96

#define IT6664_REG_ID0				0x00
#define IT6664_REG_RESET			0x0a
#define IT6664_REG_BANK				0x0f
#define IT6664_REG_08				0x08
#define IT6664_REG_0E				0x0e
#define IT6664_REG_10				0x10
#define IT6664_REG_RCLK_INTEGER			0x1e
#define IT6664_REG_RCLK_FRACTION		0x1f
#define IT6664_REG_SIPROM_ADDR_HI		0x50
#define IT6664_REG_SIPROM_ADDR_LO		0x51
#define IT6664_REG_SIPROM_COMMAND		0x54
#define IT6664_REG_SIPROM_DATA_LO		0x61
#define IT6664_REG_SIPROM_DATA_HI		0x62
#define IT6664_REG_73				0x73
#define IT6664_REG_RX_PORT0_MAP		0xf0
#define IT6664_REG_TX_COMMON_MAP		0xf1

#define IT6664_TX_REG_TIMER_LO			0x11
#define IT6664_TX_REG_TIMER_HI			0x12
#define IT6664_TX_REG_TIMER_TOP			0x13
#define IT6664_TX_REG_50				0x50

#define IT6664_SWITCH_BANK_MASK		BIT(0)
#define IT6664_RCLK_MIN_KHZ			10000U
#define IT6664_RCLK_MAX_KHZ			34000U
#define IT6664_RCLK_DEFAULT_KHZ		22000U

static const u8 it6664_expected_id[] = { 0x54, 0x49, 0x63, 0x66, 0xa0 };

static int gc573_it6664_read(struct gc573_device *dev, u8 address,
			     u8 reg, u8 *value)
{
	return gc573_i2c_read_reg(dev, address, reg, value);
}

static int gc573_it6664_write(struct gc573_device *dev, u8 address,
			      u8 reg, u8 value)
{
	return gc573_i2c_write_reg(dev, address, reg, value);
}

static int gc573_it6664_update_bits(struct gc573_device *dev, u8 address,
				    u8 reg, u8 mask, u8 value)
{
	u8 old;
	int ret;

	ret = gc573_it6664_read(dev, address, reg, &old);
	if (ret)
		return ret;

	return gc573_it6664_write(dev, address, reg,
				  (old & ~mask) | (value & mask));
}

static int gc573_it6664_write_sequence(struct gc573_device *dev, u8 address,
				       const u8 (*sequence)[2], size_t count)
{
	size_t i;
	int ret;

	for (i = 0; i < count; i++) {
		ret = gc573_it6664_write(dev, address, sequence[i][0],
					 sequence[i][1]);
		if (ret)
			return ret;
	}
	return 0;
}

static int gc573_it6664_apply_pre_rclk(struct gc573_device *dev)
{
	int ret, cleanup_ret;

	ret = gc573_it6664_write(dev, GC573_IT6664_SWITCH_ADDR,
				 IT6664_REG_RESET, 0x01);
	if (ret)
		return ret;
	ret = gc573_it6664_write(dev, GC573_IT6664_SWITCH_ADDR,
				 IT6664_REG_RESET, 0x00);
	if (ret)
		return ret;

	ret = gc573_it6664_update_bits(dev, GC573_IT6664_SWITCH_ADDR,
				       IT6664_REG_BANK,
				       IT6664_SWITCH_BANK_MASK, 0x01);
	if (ret)
		goto restore_bank;
	ret = gc573_it6664_update_bits(dev, GC573_IT6664_SWITCH_ADDR,
				       IT6664_REG_73, BIT(2), BIT(2));
	if (ret)
		goto restore_bank;
	ret = gc573_it6664_update_bits(dev, GC573_IT6664_SWITCH_ADDR,
				       IT6664_REG_BANK,
				       IT6664_SWITCH_BANK_MASK, 0x00);
	if (ret)
		goto restore_bank;

	ret = gc573_it6664_write(dev, GC573_IT6664_SWITCH_ADDR,
				 IT6664_REG_10, 0x6e);
	if (ret)
		return ret;
	ret = gc573_it6664_write(dev, GC573_IT6664_SWITCH_ADDR,
				 IT6664_REG_RX_PORT0_MAP, 0x71);
	if (ret)
		return ret;
	ret = gc573_it6664_update_bits(dev, GC573_IT6664_SWITCH_ADDR,
				       IT6664_REG_0E, 0x07, 0x00);
	if (ret)
		return ret;
	ret = gc573_it6664_update_bits(dev, GC573_IT6664_SWITCH_ADDR,
				       IT6664_REG_08, 0x0f, 0x0f);
	if (ret)
		return ret;
	ret = gc573_it6664_write(dev, GC573_IT6664_SWITCH_ADDR,
				 IT6664_REG_RX_PORT0_MAP, 0x71);
	if (ret)
		return ret;
	return gc573_it6664_write(dev, GC573_IT6664_SWITCH_ADDR,
				  IT6664_REG_TX_COMMON_MAP, 0x97);

restore_bank:
	cleanup_ret = gc573_it6664_update_bits(dev, GC573_IT6664_SWITCH_ADDR,
					       IT6664_REG_BANK,
					       IT6664_SWITCH_BANK_MASK, 0x00);
	if (cleanup_ret)
		dev_warn(&dev->pdev->dev,
			 "IT6664 switch bank restore failed: %d (operation error %d)\n",
			 cleanup_ret, ret);
	return ret ? ret : cleanup_ret;
}

static int gc573_it6664_set_siprom_address(struct gc573_device *dev,
					   u16 address)
{
	int ret;

	ret = gc573_it6664_write(dev, GC573_IT6664_SWITCH_ADDR,
				 IT6664_REG_SIPROM_ADDR_HI, (address >> 8) & 0x0f);
	if (ret)
		return ret;
	ret = gc573_it6664_write(dev, GC573_IT6664_SWITCH_ADDR,
				 IT6664_REG_SIPROM_ADDR_LO, address & 0xff);
	if (ret)
		return ret;
	return gc573_it6664_write(dev, GC573_IT6664_SWITCH_ADDR,
				  IT6664_REG_SIPROM_COMMAND, 0x04);
}

static int gc573_it6664_read_siprom_pair(struct gc573_device *dev,
					 u8 *low, u8 *high)
{
	int ret;

	ret = gc573_it6664_read(dev, GC573_IT6664_SWITCH_ADDR,
				IT6664_REG_SIPROM_DATA_LO, low);
	if (ret)
		return ret;
	return gc573_it6664_read(dev, GC573_IT6664_SWITCH_ADDR,
				 IT6664_REG_SIPROM_DATA_HI, high);
}

static int gc573_it6664_read_siprom(struct gc573_device *dev, u32 *raw)
{
	static const u8 setup[][2] = {
		{ 0xff, 0xc3 }, { 0xff, 0xa5 }, { IT6664_REG_BANK, 0x00 },
		{ 0x5f, 0x04 }, { 0x5f, 0x05 }, { 0x58, 0x12 },
		{ 0x58, 0x02 }, { 0x57, 0x01 },
		{ IT6664_REG_SIPROM_ADDR_HI, 0x00 },
		{ IT6664_REG_SIPROM_ADDR_LO, 0x00 },
		{ IT6664_REG_SIPROM_COMMAND, 0x04 },
	};
	static const u8 cleanup[][2] = {
		{ 0x5f, 0x00 }, { IT6664_REG_BANK, 0x00 }, { 0xff, 0xff },
	};
	u8 low0 = 0, high0 = 0, low1 = 0, high1 = 0;
	u16 word0, word1, address;
	int cleanup_ret = 0, step_ret, ret;
	size_t i;

	ret = gc573_it6664_write_sequence(dev, GC573_IT6664_SWITCH_ADDR,
					  setup, ARRAY_SIZE(setup));
	if (ret)
		goto out_cleanup;
	ret = gc573_it6664_read_siprom_pair(dev, &low0, &high0);
	if (ret)
		goto out_cleanup;
	ret = gc573_it6664_set_siprom_address(dev, 0x0001);
	if (ret)
		goto out_cleanup;
	ret = gc573_it6664_read_siprom_pair(dev, &low1, &high1);
	if (ret)
		goto out_cleanup;

	word0 = ((u16)high0 << 8) | low0;
	word1 = ((u16)high1 << 8) | low1;
	address = word0 == 0xffff && word1 == 0x0000 ? 0x04b0 : 0x00b0;
	ret = gc573_it6664_set_siprom_address(dev, address);
	if (ret)
		goto out_cleanup;
	ret = gc573_it6664_read_siprom_pair(dev, &low0, &high0);
	if (ret)
		goto out_cleanup;
	ret = gc573_it6664_set_siprom_address(dev, address + 1);
	if (ret)
		goto out_cleanup;
	ret = gc573_it6664_read_siprom_pair(dev, &low1, &high1);
	if (ret)
		goto out_cleanup;

	*raw = low0 | ((u32)high0 << 8) | ((u32)low1 << 16);
	if (high1 > 0xbf)
		*raw /= 100;

out_cleanup:
	/* Attempt every cleanup write; retain the operation error if one occurred. */
	for (i = 0; i < ARRAY_SIZE(cleanup); i++) {
		step_ret = gc573_it6664_write(dev, GC573_IT6664_SWITCH_ADDR,
					      cleanup[i][0], cleanup[i][1]);
		if (step_ret && !cleanup_ret)
			cleanup_ret = step_ret;
	}
	if (cleanup_ret)
		dev_warn(&dev->pdev->dev,
			 "IT6664 SIPROM cleanup failed: %d (operation error %d)\n",
			 cleanup_ret, ret);
	return ret ? ret : cleanup_ret;
}

static int gc573_it6664_calibrate_rclk(struct gc573_device *dev)
{
	u32 raw = 0, rclk_khz, timer;
	u8 fraction, integer;
	int ret;

	ret = gc573_it6664_read_siprom(dev, &raw);
	if (ret)
		return ret;
	rclk_khz = raw >= IT6664_RCLK_MIN_KHZ && raw <= IT6664_RCLK_MAX_KHZ ?
		   raw : IT6664_RCLK_DEFAULT_KHZ;
	timer = rclk_khz * 10;

	ret = gc573_it6664_write(dev, GC573_IT6664_TX_COMMON_ADDR,
				 IT6664_TX_REG_TIMER_LO, timer & 0xff);
	if (ret)
		return ret;
	ret = gc573_it6664_write(dev, GC573_IT6664_TX_COMMON_ADDR,
				 IT6664_TX_REG_TIMER_HI, (timer >> 8) & 0xff);
	if (ret)
		return ret;
	ret = gc573_it6664_update_bits(dev, GC573_IT6664_TX_COMMON_ADDR,
				       IT6664_TX_REG_TIMER_TOP, 0x03,
				       (timer >> 16) & 0x03);
	if (ret)
		return ret;

	integer = (rclk_khz / 1000) & 0x3f;
	ret = gc573_it6664_update_bits(dev, GC573_IT6664_SWITCH_ADDR,
				       IT6664_REG_RCLK_INTEGER, 0x3f,
				       integer);
	if (ret)
		return ret;
	fraction = ((rclk_khz % 1000) * 0x100) / 1000;
	ret = gc573_it6664_write(dev, GC573_IT6664_SWITCH_ADDR,
				 IT6664_REG_RCLK_FRACTION, fraction);
	if (ret)
		return ret;
	ret = gc573_it6664_update_bits(dev, GC573_IT6664_TX_COMMON_ADDR,
				       IT6664_TX_REG_50, BIT(2), 0);
	if (ret)
		return ret;

	{
		static const u8 post_sequence[][2] = {
			{ 0x2c, 0x69 }, { 0x2d, 0x6b },
			{ 0x2e, 0x6d }, { 0x2f, 0x6f },
		};

		ret = gc573_it6664_write_sequence(dev,
						  GC573_IT6664_TX_COMMON_ADDR,
						  post_sequence,
						  ARRAY_SIZE(post_sequence));
	}
	if (ret)
		return ret;

	dev_info(&dev->pdev->dev,
		 "IT6664 RCLK SIPROM raw=%u kHz selected=%u kHz%s\n",
		 raw, rclk_khz,
		 raw >= IT6664_RCLK_MIN_KHZ && raw <= IT6664_RCLK_MAX_KHZ ?
		 "" : " (default)");
	return 0;
}

int gc573_bridge_clock_init(struct gc573_device *dev)
{
	u8 id[ARRAY_SIZE(it6664_expected_id)];
	int ret;

	if (!dev || !dev->pdev)
		return -EINVAL;

	ret = gc573_i2c_read_block(dev, GC573_IT6664_SWITCH_ADDR, IT6664_REG_ID0,
				   id, sizeof(id));
	if (ret)
		return ret;
	if (memcmp(id, it6664_expected_id, sizeof(id)))
		return -ENODEV;

	ret = gc573_it6664_apply_pre_rclk(dev);
	if (ret)
		return ret;
	return gc573_it6664_calibrate_rclk(dev);
}
