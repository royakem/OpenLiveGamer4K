// SPDX-License-Identifier: GPL-2.0-only
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/types.h>

#include "gc573_pure.h"
#include "gc573_bridge_rx.h"

/*
 * Provenance: RX address/register conventions and the CAOF/register/power
 * sequence are ported from
 * ostrich/gc555: gc555-it6664-core.c:
 * it6664_initialize_rx_caof(), it6664_initialize_rx_registers(), and
 * it6664_initialize_rx_power_state(). The exact RX ID below is the recorded
 * board read at 8-bit address 0x70 in evidence/bridge-baseline-20260924.txt.
 */
#define GC573_IT6664_RX_ADDRESS       0x70
#define GC573_IT6664_RX_REG_BANK      0x0f
#define GC573_IT6664_RX_BANK_MASK     GENMASK(1, 0)
#define GC573_IT6664_RX_BANK_0        0x00
#define GC573_IT6664_RX_BANK_3        0x03
#define GC573_IT6664_RX_CAOF_STATUS   0x08
#define GC573_IT6664_RX_CAOF_POLLS    0x1f
#define GC573_IT6664_RX_CAOF_DONE     GENMASK(5, 4)
#define GC573_IT6664_RX_ID0            0x54
#define GC573_IT6664_RX_ID1            0x49
#define GC573_IT6664_RX_ID2            0x64
#define GC573_IT6664_RX_ID3            0x66

struct gc573_bridge_rx_update {
	u8 reg;
	u8 mask;
	u8 value;
};

struct gc573_bridge_rx_write {
	u8 reg;
	u8 value;
};

static int gc573_bridge_rx_read(struct gc573_device *dev, u8 reg, u8 *value)
{
	return gc573_i2c_read_reg(dev, GC573_IT6664_RX_ADDRESS, reg, value);
}

static int gc573_bridge_rx_write(struct gc573_device *dev, u8 reg, u8 value)
{
	return gc573_i2c_write_reg(dev, GC573_IT6664_RX_ADDRESS, reg, value);
}

static int gc573_bridge_rx_update(struct gc573_device *dev, u8 reg,
				  u8 mask, u8 value)
{
	u8 old_value;
	int ret;

	ret = gc573_bridge_rx_read(dev, reg, &old_value);
	if (ret)
		return ret;

	return gc573_bridge_rx_write(dev, reg,
				     (old_value & ~mask) | (value & mask));
}

static int gc573_bridge_rx_updates(struct gc573_device *dev,
				   const struct gc573_bridge_rx_update *updates,
				   size_t count)
{
	size_t i;
	int ret;

	for (i = 0; i < count; i++) {
		ret = gc573_bridge_rx_update(dev, updates[i].reg,
					     updates[i].mask, updates[i].value);
		if (ret)
			return ret;
	}

	return 0;
}

static int gc573_bridge_rx_writes(struct gc573_device *dev,
				  const struct gc573_bridge_rx_write *writes,
				  size_t count)
{
	size_t i;
	int ret;

	for (i = 0; i < count; i++) {
		ret = gc573_bridge_rx_write(dev, writes[i].reg,
					    writes[i].value);
		if (ret)
			return ret;
	}

	return 0;
}

static int gc573_bridge_rx_select_bank(struct gc573_device *dev, u8 bank)
{
	return gc573_bridge_rx_update(dev, GC573_IT6664_RX_REG_BANK,
				      GC573_IT6664_RX_BANK_MASK, bank);
}

static int gc573_bridge_rx_check_identity(struct gc573_device *dev)
{
	static const u8 expected[] = {
		GC573_IT6664_RX_ID0, GC573_IT6664_RX_ID1,
		GC573_IT6664_RX_ID2, GC573_IT6664_RX_ID3,
	};
	u8 identity[ARRAY_SIZE(expected)];
	unsigned int i;
	int ret;

	for (i = 0; i < ARRAY_SIZE(expected); i++) {
		ret = gc573_bridge_rx_read(dev, i, &identity[i]);
		if (ret)
			return ret;
	}

	for (i = 0; i < ARRAY_SIZE(expected); i++) {
		if (identity[i] != expected[i])
			return -ENODEV;
	}

	return 0;
}

static const struct gc573_bridge_rx_write gc573_bridge_rx_reset[] = {
	{ 0x22, 0x08 },
	{ 0x23, 0x01 },
	{ 0x22, 0x17 },
	{ 0x24, 0xf8 },
	{ 0x23, 0xa0 },
	{ 0x22, 0x00 },
	{ 0x24, 0x00 },
};

static const struct gc573_bridge_rx_update gc573_bridge_rx_caof_prefix[] = {
	{ 0x0f, 0x03, 0x00 },
	{ 0x29, BIT(0), BIT(0) },
	{ 0x2a, BIT(6) | BIT(0), BIT(6) | BIT(0) },
	{ 0x0f, 0x03, 0x03 },
	{ 0x3a, BIT(7), 0 },
	{ 0x3b, GENMASK(7, 6), 0 },
	{ 0xa0, BIT(7), BIT(7) },
	{ 0xa1, BIT(7), BIT(7) },
	{ 0xa2, BIT(7), BIT(7) },
	{ 0xa7, BIT(4), BIT(4) },
	{ 0x48, BIT(7), BIT(7) },
	{ 0x0f, 0x03, 0x00 },
	{ 0x2a, BIT(6), 0 },
	{ 0x24, BIT(2), BIT(2) },
};

static const struct gc573_bridge_rx_write gc573_bridge_rx_caof_zero[] = {
	{ 0x25, 0x00 }, { 0x26, 0x00 }, { 0x27, 0x00 }, { 0x28, 0x00 },
};

static const struct gc573_bridge_rx_update gc573_bridge_rx_caof_tail[] = {
	{ 0x3c, BIT(4), 0 },
	{ 0x0f, 0x03, 0x03 },
	{ 0x3a, BIT(7), BIT(7) },
	{ 0x0f, 0x03, 0x00 },
};

static const struct gc573_bridge_rx_update gc573_bridge_rx_caof_finish[] = {
	{ 0x3a, BIT(7), 0 },
	{ 0xa0, BIT(7), 0 },
	{ 0xa1, BIT(7), 0 },
	{ 0xa2, BIT(7), 0 },
	{ 0x0f, 0x03, 0x00 },
	{ 0x08, GENMASK(5, 4), GENMASK(5, 4) },
	{ 0x29, BIT(0), 0 },
	{ 0x24, BIT(2), 0 },
	{ 0x3c, BIT(4), BIT(4) },
	{ 0xce, BIT(5), 0 },
};

static int gc573_bridge_rx_caof_poll(struct gc573_device *dev)
{
	unsigned int attempt;
	u8 status;
	int ret;
	int cleanup_ret;

	for (attempt = 0; attempt < GC573_IT6664_RX_CAOF_POLLS; attempt++) {
		ret = gc573_bridge_rx_read(dev, GC573_IT6664_RX_CAOF_STATUS,
					   &status);
		if (ret)
			return ret;
		/* An absent/unresponsive endpoint can read back as all ones. */
		if (status != 0xff && (status & GC573_IT6664_RX_CAOF_DONE))
			return 0;
		if (attempt > 2) {
			ret = gc573_bridge_rx_update(dev, 0x2a, BIT(6), BIT(6));
			if (ret)
				return ret;
			ret = gc573_bridge_rx_update(dev, 0x2a, BIT(6), 0);
			if (ret)
				return ret;
		}
	}

	/* Match the reference's bounded recovery writes, but keep timeout fatal. */
	ret = gc573_bridge_rx_select_bank(dev, GC573_IT6664_RX_BANK_3);
	if (ret)
		return ret;
	ret = gc573_bridge_rx_update(dev, 0x3a, BIT(7), 0);
	if (ret)
		return ret;
	ret = gc573_bridge_rx_select_bank(dev, GC573_IT6664_RX_BANK_0);
	if (ret)
		return ret;
	ret = gc573_bridge_rx_update(dev, 0x2a, BIT(6), BIT(6));
	if (ret)
		return ret;
	ret = gc573_bridge_rx_update(dev, 0x2a, BIT(6), 0);
	if (ret)
		return ret;

	cleanup_ret = gc573_bridge_rx_select_bank(dev, GC573_IT6664_RX_BANK_0);
	return cleanup_ret ? cleanup_ret : -ETIMEDOUT;
}

static int gc573_bridge_rx_caof(struct gc573_device *dev)
{
	u8 discard;
	int ret;
	int cleanup_ret;

	ret = gc573_bridge_rx_writes(dev, gc573_bridge_rx_reset, 4);
	if (ret)
		goto cleanup;
	usleep_range(10000, 11000);
	ret = gc573_bridge_rx_writes(dev, &gc573_bridge_rx_reset[4],
				     ARRAY_SIZE(gc573_bridge_rx_reset) - 4);
	if (ret)
		goto cleanup;
	ret = gc573_bridge_rx_updates(dev, gc573_bridge_rx_caof_prefix,
				      ARRAY_SIZE(gc573_bridge_rx_caof_prefix));
	if (ret)
		goto cleanup;
	ret = gc573_bridge_rx_writes(dev, gc573_bridge_rx_caof_zero,
				     ARRAY_SIZE(gc573_bridge_rx_caof_zero));
	if (ret)
		goto cleanup;
	ret = gc573_bridge_rx_updates(dev, gc573_bridge_rx_caof_tail,
				      ARRAY_SIZE(gc573_bridge_rx_caof_tail));
	if (ret)
		goto cleanup;
	ret = gc573_bridge_rx_caof_poll(dev);
	if (ret)
		goto cleanup;

	ret = gc573_bridge_rx_select_bank(dev, GC573_IT6664_RX_BANK_3);
	if (ret)
		goto cleanup;
	ret = gc573_bridge_rx_read(dev, 0x5a, &discard);
	if (ret)
		goto cleanup;
	ret = gc573_bridge_rx_read(dev, 0x59, &discard);
	if (ret)
		goto cleanup;
	ret = gc573_bridge_rx_read(dev, 0x59, &discard);
	if (ret)
		goto cleanup;
	ret = gc573_bridge_rx_updates(dev, gc573_bridge_rx_caof_finish,
				      ARRAY_SIZE(gc573_bridge_rx_caof_finish));

cleanup:
	cleanup_ret = gc573_bridge_rx_select_bank(dev,
						 GC573_IT6664_RX_BANK_0);
	return ret ? ret : cleanup_ret;
}

static int gc573_bridge_rx_registers(struct gc573_device *dev)
{
	int ret;
	int cleanup_ret;

	ret = gc573_bridge_rx_write(dev, 0x56, 0xff);
	if (ret)
		goto cleanup;
	ret = gc573_bridge_rx_write(dev, 0x57, 0xff);
	if (ret)
		goto cleanup;
	ret = gc573_bridge_rx_select_bank(dev, GC573_IT6664_RX_BANK_3);
	if (ret)
		goto cleanup;
	ret = gc573_bridge_rx_update(dev, 0xa8, BIT(3), BIT(3));
	if (ret)
		goto cleanup;
	ret = gc573_bridge_rx_update(dev, 0xa7, BIT(6), BIT(6));
	if (ret)
		goto cleanup;
	ret = gc573_bridge_rx_update(dev, 0x26, BIT(5), 0);
	if (ret)
		goto cleanup;
	ret = gc573_bridge_rx_write(dev, 0x27, 0x9f);
	if (ret)
		goto cleanup;
	ret = gc573_bridge_rx_write(dev, 0x28, 0x9f);
	if (ret)
		goto cleanup;
	ret = gc573_bridge_rx_write(dev, 0x29, 0x9f);
	if (ret)
		goto cleanup;
	ret = gc573_bridge_rx_select_bank(dev, GC573_IT6664_RX_BANK_0);
	if (ret)
		goto cleanup;
	ret = gc573_bridge_rx_update(dev, 0x28, 0x59, 0x59);
	if (ret)
		goto cleanup;
	ret = gc573_bridge_rx_update(dev, 0x2a, BIT(0), BIT(0));
	if (ret)
		goto cleanup;
	ret = gc573_bridge_rx_update(dev, 0x43, BIT(1), 0);
	if (ret)
		goto cleanup;
	ret = gc573_bridge_rx_update(dev, 0x44, GENMASK(5, 0), 0x19);
	if (ret)
		goto cleanup;
	ret = gc573_bridge_rx_update(dev, 0x3c, BIT(0), 0);
	if (ret)
		goto cleanup;
	ret = gc573_bridge_rx_write(dev, 0x45, 0xdf);
	if (ret)
		goto cleanup;
	ret = gc573_bridge_rx_update(dev, 0x46, GENMASK(5, 0), 0x15);
	if (ret)
		goto cleanup;
	ret = gc573_bridge_rx_update(dev, 0x47, 0xff, 0x88);
	if (ret)
		goto cleanup;
	ret = gc573_bridge_rx_write(dev, 0x49, 0xe1);
	if (ret)
		goto cleanup;
	ret = gc573_bridge_rx_write(dev, 0x23, 0xa0);
	if (ret)
		goto cleanup;
	ret = gc573_bridge_rx_write(dev, 0x53, 0x0f);
	if (ret)
		goto cleanup;
	ret = gc573_bridge_rx_write(dev, 0xe3, 0x04);
	if (ret)
		goto cleanup;
	ret = gc573_bridge_rx_update(dev, 0xce, BIT(7), 0);
	if (ret)
		goto cleanup;
	ret = gc573_bridge_rx_update(dev, 0x3c, BIT(5), 0);
	if (ret)
		goto cleanup;
	ret = gc573_bridge_rx_select_bank(dev, GC573_IT6664_RX_BANK_3);
	if (ret)
		goto cleanup;
	ret = gc573_bridge_rx_update(dev, 0xe3, BIT(0), BIT(0));
	if (ret)
		goto cleanup;
	ret = gc573_bridge_rx_update(dev, 0xe3, BIT(2) | BIT(1), 0x03);
	if (ret)
		goto cleanup;
	ret = gc573_bridge_rx_write(dev, 0xf0, 0xa0);
	if (ret)
		goto cleanup;
	ret = gc573_bridge_rx_select_bank(dev, GC573_IT6664_RX_BANK_0);
	if (ret)
		goto cleanup;
	ret = gc573_bridge_rx_update(dev, 0x28, BIT(7) | BIT(3),
				    BIT(7) | BIT(3));
	if (ret)
		goto cleanup;
	ret = gc573_bridge_rx_update(dev, 0x3b, BIT(5), BIT(5));
	if (ret)
		goto cleanup;
	ret = gc573_bridge_rx_write(dev, 0x26, 0xff);
	if (ret)
		goto cleanup;
	ret = gc573_bridge_rx_update(dev, 0x42, BIT(5), 0);

cleanup:
	cleanup_ret = gc573_bridge_rx_select_bank(dev,
						 GC573_IT6664_RX_BANK_0);
	return ret ? ret : cleanup_ret;
}

/*
 * Retain the reference's RX-side output quiesce/reset writes. Switch reset
 * and the reference's runtime-dependent HPD/EDID programming belong to the
 * switch bootstrap and parent HDMI/EDID owner, so they are deliberately absent.
 */
static int gc573_bridge_rx_power_state(struct gc573_device *dev)
{
	static const struct gc573_bridge_rx_update quiesce_updates[] = {
		{ 0x53, 0xe0, 0 },
		{ 0x54, 0xff, 0 },
		{ 0x55, 0x07, 0 },
		{ 0x57, 0x0f, 0 },
	};
	int ret;
	int cleanup_ret;

	ret = gc573_bridge_rx_updates(dev, quiesce_updates,
				      ARRAY_SIZE(quiesce_updates));
	if (ret)
		goto cleanup;
	ret = gc573_bridge_rx_update(dev, 0xc5, BIT(4), BIT(4));
	if (ret)
		goto cleanup;
	ret = gc573_bridge_rx_update(dev, 0xc5, BIT(4), 0);
	if (ret)
		goto cleanup;
	ret = gc573_bridge_rx_select_bank(dev, GC573_IT6664_RX_BANK_3);
	if (ret)
		goto cleanup;
	ret = gc573_bridge_rx_write(dev, 0x27, 0x9f);
	if (ret)
		goto cleanup;
	ret = gc573_bridge_rx_write(dev, 0x28, 0x9f);
	if (ret)
		goto cleanup;
	ret = gc573_bridge_rx_write(dev, 0x29, 0x9f);
	if (ret)
		goto cleanup;
	ret = gc573_bridge_rx_write(dev, 0x20, 0x1b);
	if (ret)
		goto cleanup;
	ret = gc573_bridge_rx_write(dev, 0x21, 0x03);

cleanup:
	cleanup_ret = gc573_bridge_rx_select_bank(dev,
						 GC573_IT6664_RX_BANK_0);
	return ret ? ret : cleanup_ret;
}

int gc573_bridge_rx_init(struct gc573_device *dev)
{
	int ret;
	int cleanup_ret;

	if (!dev)
		return -EINVAL;

	/* Guard all writes and start the CAOF calibration from the known bank. */
	ret = gc573_bridge_rx_check_identity(dev);
	if (ret)
		return ret;
	ret = gc573_bridge_rx_select_bank(dev, GC573_IT6664_RX_BANK_0);
	if (ret)
		goto cleanup;
	ret = gc573_bridge_rx_caof(dev);
	if (!ret)
		ret = gc573_bridge_rx_registers(dev);
	if (!ret)
		ret = gc573_bridge_rx_power_state(dev);

cleanup:
	cleanup_ret = gc573_bridge_rx_select_bank(dev,
						 GC573_IT6664_RX_BANK_0);
	return ret ? ret : cleanup_ret;
}
