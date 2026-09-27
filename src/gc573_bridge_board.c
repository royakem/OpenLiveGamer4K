// SPDX-License-Identifier: GPL-2.0-only
/* Shared switch setup adapted from GPL GC555 it6664_configure_tx_switch(),
 * it6664_initialize_tx_reset_path(), and it6664_write_upstream_edid().
 * TX1 downstream EDID and upstream RAM independently measured on GC573.
 * Experimental exclusive probe-time owner; not a hotplug/runtime service.
 */
#include <linux/delay.h>
#include <linux/string.h>
#include "gc573_pure.h"
#include "gc573_edid.h"
#include "gc573_bridge_tx.h"
#include "gc573_bridge_board.h"

static int update(struct gc573_device *dev, u8 addr, u8 reg, u8 mask, u8 value)
{
	u8 old;
	int ret = gc573_i2c_read_reg(dev, addr, reg, &old);
	if (ret)
		return ret;
	return gc573_i2c_write_reg(dev, addr, reg, (old & ~mask) | (value & mask));
}

struct step { u8 addr, reg, mask, value; };
static int sequence(struct gc573_device *dev, const struct step *steps, size_t count)
{
	size_t i;
	int ret;
	for (i = 0; i < count; i++) {
		ret = steps[i].mask == 0xff ?
			gc573_i2c_write_reg(dev, steps[i].addr, steps[i].reg, steps[i].value) :
			update(dev, steps[i].addr, steps[i].reg, steps[i].mask, steps[i].value);
		if (ret)
			return ret;
	}
	return 0;
}

int gc573_bridge_board_prepare(struct gc573_device *dev)
{
	static const struct step reset[] = {
		{0x70, 0x0f, 3, 0}, {0x70, 0xc5, 1, 1},
		{0x96, 0x20, 0xff, 2}, {0x96, 0x20, 0xff, 0},
		{0x6a, 0xc1, 1, 1}, {0x6a, 0x01, 1, 1}, {0x6a, 0x01, 1, 0},
	};
	static const struct step route[] = {
		{0x58, 0x0f, 1, 0},
		{0x58, 0x0c, 0x20, 0x20}, {0x58, 0x0c, 0x20, 0},
		{0x96, 0x15, 8, 8},
		{0x68, 0x03, 0xff, 3}, {0x68, 0x84, 0xff, 0x60},
		{0x68, 0x86, 0xff, 0}, {0x68, 0x88, 0xff, 0x0b},
		{0x6e, 0x84, 0xff, 0x60}, {0x6e, 0x86, 0xff, 0},
		{0x6e, 0x88, 0xff, 0x0b},
		{0x58, 0x08, 0x0f, 0x0f}, {0x58, 0x0d, 0xff, 0},
		{0x58, 0x6b, 0x3c, 0}, {0x58, 0x6c, 0x38, 0x20},
		{0x58, 0x18, 0x10, 0x10}, {0x58, 0x0f, 1, 1},
		{0x58, 0x10, 0x49, 0x41}, {0x58, 0x1d, 0x80, 0x80},
		{0x58, 0x20, 0x78, 0x78}, {0x58, 0x0f, 1, 0},
		{0x58, 0x19, 0x3f, 0x0f}, {0x58, 0x2b, 0xff, 0xff},
		{0x58, 0x2d, 0xff, 0x0f}, {0x58, 0x2e, 0xff, 0xff},
		{0x58, 0x30, 0xff, 0x0f}, {0x58, 0x6d, 0x30, 0},
	};
	unsigned int port;
	int ret, cleanup;

	/* Missing shared portion of the reference RX quiesce closure. */
	ret = update(dev, 0x58, 0x0a, 4, 4);
	if (ret)
		goto out;
	usleep_range(1000, 2000);
	ret = update(dev, 0x58, 0x0a, 4, 0);
	if (ret)
		goto out;
	ret = sequence(dev, reset, ARRAY_SIZE(reset));
	if (ret)
		goto out;
	ret = gc573_bridge_tx_prepare(dev, 1);
	if (ret)
		goto out;
	ret = sequence(dev, route, ARRAY_SIZE(route));
	if (ret)
		goto out;
	usleep_range(10000, 11000);
	for (port = 0; port < 4; port++) {
		u8 addr = 0x68 + 2 * port;
		ret = update(dev, addr, 0x41, 1, 0);
		if (ret)
			goto out;
		ret = update(dev, addr, 0xc1, 1, 1);
		if (ret)
			goto out;
		ret = update(dev, addr, 0x88, 1, 1);
		if (ret)
			goto out;
	}
out:
	cleanup = update(dev, 0x58, 0x0f, 1, 0);
	return ret ? ret : cleanup;
}

int gc573_bridge_program_edid(struct gc573_device *dev)
{
	u8 edid[256], readback[127];
	unsigned int block, i;
	int ret, cleanup;
	static const struct step prepare[] = {
		{0x70, 0x0f, 3, 0}, {0x70, 0x34, 1, 0},
		{0x70, 0xc6, 0xff, 0}, {0x70, 0xc7, 0xff, 0x10},
		{0x70, 0xc8, 0xff, 0}, {0x70, 0x4b, 0xff, 0xd9},
	};
	ret = gc573_it6805_get_edid(edid, sizeof(edid));
	if (ret)
		return ret;
	ret = sequence(dev, prepare, ARRAY_SIZE(prepare));
	if (ret)
		goto out;
	for (block = 0; block < 2; block++) {
		for (i = 0; i < 127; i++) {
			ret = gc573_i2c_write_reg(dev, 0xd8, block * 128 + i,
						edid[block * 128 + i]);
			if (ret)
				goto out;
		}
		ret = gc573_i2c_read_block(dev, 0xd8, block * 128, readback, sizeof(readback));
		if (ret)
			goto out;
		if (memcmp(readback, edid + block * 128, sizeof(readback))) {
			ret = -EIO;
			goto out;
		}
		ret = gc573_i2c_write_reg(dev, 0x70, 0xc9 + block, edid[block * 128 + 127]);
		if (ret)
			goto out;
	}
	ret = update(dev, 0x58, 0x0f, 1, 1);
	if (ret)
		goto out;
	ret = update(dev, 0x58, 0x10, 0x40, 0x40);
	if (ret)
		goto out;
	ret = update(dev, 0x70, 0xc5, 1, 0);
out:
	cleanup = update(dev, 0x58, 0x0f, 1, 0);
	if (!ret)
		ret = cleanup;
	cleanup = update(dev, 0x70, 0x34, 1, 1);
	if (!ret)
		ret = cleanup;
	if (!ret)
		dev_info(&dev->pdev->dev, "IT6664 upstream EDID programmed/readback verified checksums=%02x/%02x\n", edid[127], edid[255]);
	return ret;
}
