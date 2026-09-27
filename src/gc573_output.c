// SPDX-License-Identifier: GPL-2.0-only
#include <linux/bitops.h>
#include <linux/errno.h>
#include <linux/types.h>

#include "gc573_output.h"
#include "gc573_pure.h"

#define GC573_IT6805_ADDRESS       0x90
#define GC573_IT6805_REG_BANK      0x0f
#define GC573_IT6805_BANK_MASK     GENMASK(2, 0)
#define GC573_IT6805_BANK_0        0x00
#define GC573_IT6805_BANK_1        0x01
#define GC573_IT6805_BANK_2        0x02
#define GC573_IT6805_BANK_5        0x05

static int gc573_it6805_output_read(struct gc573_device *dev, u8 reg,
				    u8 *value)
{
	return gc573_i2c_read_reg(dev, GC573_IT6805_ADDRESS, reg, value);
}

static int gc573_it6805_output_write(struct gc573_device *dev, u8 reg,
				     u8 value)
{
	return gc573_i2c_write_reg(dev, GC573_IT6805_ADDRESS, reg, value);
}

static int gc573_it6805_output_update_bits(struct gc573_device *dev, u8 reg,
					   u8 mask, u8 value)
{
	u8 old_value;
	int ret;

	ret = gc573_it6805_output_read(dev, reg, &old_value);
	if (ret)
		return ret;

	return gc573_it6805_output_write(dev, reg,
					 (old_value & ~mask) | (value & mask));
}

static int gc573_it6805_output_set_bank(struct gc573_device *dev, u8 bank)
{
	if (bank > GC573_IT6805_BANK_MASK)
		return -EINVAL;

	return gc573_it6805_output_update_bits(dev, GC573_IT6805_REG_BANK,
						GC573_IT6805_BANK_MASK, bank);
}

static int gc573_it6805_output_restore_bank0(struct gc573_device *dev,
						     int status)
{
	int restore_status;

	restore_status = gc573_it6805_output_set_bank(dev,
						       GC573_IT6805_BANK_0);
	return status ? status : restore_status;
}

int gc573_it6805_configure_output(struct gc573_device *dev)
{
	u8 timing[9];
	u8 avi[6];
	u32 width;
	u32 height;
	u8 colorspace;
	bool high_bandwidth;
	int ret;

	if (!dev)
		return -EINVAL;

	/* Qualified RGB8 input families; profile follows the GPL IT6805 output path. */
	ret = gc573_it6805_output_set_bank(dev, GC573_IT6805_BANK_0);
	if (ret)
		goto out;
	ret = gc573_i2c_read_block(dev, GC573_IT6805_ADDRESS, 0x9d,
				   timing, sizeof(timing));
	if (ret)
		goto out;
	width = (timing[0] | (timing[1] << 8)) & 0x3fff;
	height = (timing[7] | (timing[8] << 8)) & 0x3fff;
	if (!((width == 3840 && height == 2160) ||
	      (width == 1920 && height == 1080) ||
	      (width == 1280 && (height == 720 || height == 800)))) {
		ret = -EOPNOTSUPP;
		goto out;
	}

	high_bandwidth = width == 3840;

	/* GC555 reads AVI InfoFrame bytes 0x14..0x19 from bank 2. */
	ret = gc573_it6805_output_set_bank(dev, GC573_IT6805_BANK_2);
	if (ret)
		goto out;
	ret = gc573_i2c_read_block(dev, GC573_IT6805_ADDRESS, 0x14,
				   avi, sizeof(avi));
	if (ret)
		goto out;
	ret = gc573_it6805_output_set_bank(dev, GC573_IT6805_BANK_0);
	if (ret)
		goto out;
	colorspace = (avi[1] >> 5) & 0x03;
	if (colorspace != 0) {
		ret = -EOPNOTSUPP;
		goto out;
	}

	/* Minimal measured GC573 native-4K output profile. Receiver calibration
	 * and the input path are initialized by the caller. The working bank-1
	 * readback is c0=43,c1=02,c4=10,c5=7f,c6=00. */
	ret = gc573_it6805_output_set_bank(dev, GC573_IT6805_BANK_1);
	if (ret)
		goto out;
	ret = gc573_it6805_output_update_bits(dev, 0xc0, 0x07, high_bandwidth ? 0x03 : 0x02);
	if (ret)
		goto out;
	ret = gc573_it6805_output_update_bits(dev, 0xc1, 0x22, high_bandwidth ? 0x02 : 0x00);
	if (ret)
		goto out;
	/* High-bandwidth LVDS path from the source reference. A working GC573
	 * readback confirms c0=43,c1=02,c4=10,c5=7f,c6=00 at native 4K. */
	ret = gc573_it6805_output_set_bank(dev, GC573_IT6805_BANK_5);
	if (ret)
		goto out;
	ret = gc573_it6805_output_update_bits(dev, 0xd1, 0x0d, BIT(2));
	if (ret)
		goto out;
	ret = gc573_it6805_output_update_bits(dev, 0xda, BIT(4), 0);
	if (ret)
		goto out;
	ret = gc573_it6805_output_write(dev, 0xd0, 0xf3);
	if (ret)
		goto out;
	ret = gc573_it6805_output_set_bank(dev, GC573_IT6805_BANK_1);
	if (ret)
		goto out;
	ret = gc573_it6805_output_update_bits(dev, 0xbd, 0x30, high_bandwidth ? BIT(4) : 0);
	if (ret)
		goto out;
	ret = gc573_it6805_output_write(dev, 0xbe, 0);
	if (ret)
		goto out;
	ret = gc573_it6805_output_update_bits(dev, 0xfe, BIT(4), BIT(4));
	if (ret)
		goto out;
	ret = gc573_it6805_output_update_bits(dev, 0xb0, BIT(0), high_bandwidth ? BIT(0) : 0);
	if (ret)
		goto out;
	ret = gc573_it6805_output_write(dev, 0xc4, high_bandwidth ? 0x10 : 0);
	if (ret)
		goto out;
	/* RGB8 single-pixel c0=0x42 releases the TTL pins with c5=0x18;
	 * dual-pixel output uses c6=0. Reference release_video_tristate. */
	ret = gc573_it6805_output_write(dev, 0xc5, high_bandwidth ? 0x7f : 0x18);
	if (ret)
		goto out;
	ret = gc573_it6805_output_write(dev, 0xc6, 0x00);

out:
	return gc573_it6805_output_restore_bank0(dev, ret);
}
