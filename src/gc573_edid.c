// SPDX-License-Identifier: GPL-2.0-only
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/mutex.h>
#include <linux/string.h>
#include <linux/delay.h>

#include "gc573_pure.h"
#include "gc573_edid.h"

/* These are even 8-bit bus address bytes used by the board transaction API. */
#define GC573_IT6805_ADDRESS          0x90
#define GC573_IT6805_EDID_ADDRESS     0xa8

#define GC573_IT6805_REG_BANK         0x0f
#define GC573_IT6805_BANK_MASK        GENMASK(2, 0)
#define GC573_IT6805_BANK_0           0x00
#define GC573_IT6805_BANK_PORT1       0x04

#define GC573_IT6805_EDID_DATA_REG    0xa8
#define GC573_IT6805_EDID_BLOCK_SIZE  128
#define GC573_IT6805_EDID_BLOCKS      2
#define GC573_IT6805_EDID_SIZE        (GC573_IT6805_EDID_BLOCK_SIZE * \
					  GC573_IT6805_EDID_BLOCKS)

/* Captured verbatim from evidence/receiver-edid-20260924.bin. */
static const u8 gc573_it6805_captured_edid[GC573_IT6805_EDID_SIZE] = {
	0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x06, 0xd8, 0x42, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x24, 0x1c, 0x01, 0x03, 0x80, 0xa0, 0x5a, 0x78, 0xea, 0x08, 0xa5, 0xa2, 0x57, 0x4f, 0xa2, 0x28,
	0x0f, 0x50, 0x54, 0x25, 0x0b, 0x00, 0xd1, 0xc0, 0x81, 0xc0, 0x81, 0x80, 0x81, 0x00, 0x8b, 0xc0,
	0x95, 0x00, 0xb3, 0x00, 0x3b, 0x80, 0x08, 0xe8, 0x00, 0x30, 0xf2, 0x70, 0x5a, 0x80, 0xb0, 0x58,
	0x8a, 0x00, 0x6d, 0x55, 0x21, 0x00, 0x00, 0x1e, 0x02, 0x3a, 0x80, 0x18, 0x71, 0x38, 0x2d, 0x40,
	0x58, 0x2c, 0x45, 0x00, 0x40, 0x84, 0x63, 0x00, 0x00, 0x1e, 0x00, 0x00, 0x00, 0xfc, 0x00, 0x41,
	0x56, 0x54, 0x20, 0x43, 0x4c, 0x35, 0x31, 0x31, 0x2d, 0x48, 0x4e, 0x0a, 0x00, 0x00, 0x00, 0xfd,
	0x00, 0x32, 0xf0, 0x1e, 0xde, 0x3c, 0x00, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x01, 0x00,
	0x02, 0x03, 0x50, 0x70, 0x5e, 0x61, 0x60, 0x66, 0x65, 0x5f, 0x5e, 0x5d, 0x10, 0x1f, 0x5a, 0x3f,
	0x05, 0x14, 0x04, 0x13, 0x12, 0x11, 0x03, 0x02, 0x01, 0x22, 0x21, 0x20, 0x16, 0x15, 0x07, 0x06,
	0x62, 0x63, 0x64, 0x23, 0x0f, 0x07, 0x07, 0x83, 0x4f, 0x00, 0x00, 0x6e, 0x03, 0x0c, 0x00, 0x10,
	0x00, 0x38, 0x3c, 0x20, 0x00, 0x80, 0x01, 0x02, 0x03, 0x04, 0x67, 0xd8, 0x5d, 0xc4, 0x01, 0x78,
	0x80, 0x03, 0xe2, 0x00, 0x4f, 0xe3, 0x05, 0xc0, 0x00, 0xe2, 0x0f, 0x0f, 0xe3, 0x06, 0x05, 0x01,
	0x56, 0x5e, 0x00, 0xa0, 0xa0, 0xa0, 0x29, 0x50, 0x30, 0x20, 0x35, 0x00, 0x55, 0x50, 0x21, 0x00,
	0x00, 0x1e, 0xfc, 0x7e, 0x80, 0x88, 0x70, 0x38, 0x12, 0x40, 0x18, 0x20, 0x35, 0x00, 0x20, 0x2f,
	0x21, 0x00, 0x00, 0x1e, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

int gc573_it6805_get_edid(u8 *edid, size_t size)
{
	unsigned int block, i;
	u8 sum;

	if (!edid || size != sizeof(gc573_it6805_captured_edid))
		return -EINVAL;
	memcpy(edid, gc573_it6805_captured_edid, size);
	for (block = 0; block < 2; block++) {
		sum = 0;
		for (i = 0; i < 127; i++)
			sum += edid[block * 128 + i];
		edid[block * 128 + 127] = -sum;
	}
	return 0;
}


static int gc573_edid_set_bank(struct gc573_device *dev, u8 bank)
{
	u8 value;
	int ret;

	if (bank > GC573_IT6805_BANK_MASK)
		return -EINVAL;
	ret = gc573_i2c_read_reg(dev, GC573_IT6805_ADDRESS,
				 GC573_IT6805_REG_BANK, &value);
	if (ret)
		return ret;
	value = (value & ~GC573_IT6805_BANK_MASK) | bank;
	return gc573_i2c_write_reg(dev, GC573_IT6805_ADDRESS,
				   GC573_IT6805_REG_BANK, value);
}

static int gc573_edid_write_block(struct gc573_device *dev,
				  unsigned int block, u8 *checksum)
{
	unsigned int base = block * GC573_IT6805_EDID_BLOCK_SIZE;
	u8 sum = 0;
	unsigned int i;
	int ret;

	if (block >= GC573_IT6805_EDID_BLOCKS || !checksum)
		return -EINVAL;

	for (i = 0; i < GC573_IT6805_EDID_BLOCK_SIZE - 1; i++) {
		u8 value = gc573_it6805_captured_edid[base + i];

		ret = gc573_i2c_write_reg(dev, GC573_IT6805_EDID_ADDRESS,
					  base + i, value);
		if (ret)
			return ret;
		sum += value;
	}
	*checksum = (u8)-sum;
	return 0;
}

static int gc573_edid_verify(struct gc573_device *dev)
{
	u8 readback[GC573_IT6805_EDID_BLOCK_SIZE - 1];
	unsigned int block;
	int ret;

	for (block = 0; block < GC573_IT6805_EDID_BLOCKS; block++) {
		u8 base = block * GC573_IT6805_EDID_BLOCK_SIZE;

		ret = gc573_i2c_read_block(dev, GC573_IT6805_EDID_ADDRESS,
					   base, readback, sizeof(readback));
		if (ret)
			return ret;
		if (memcmp(readback, &gc573_it6805_captured_edid[base],
			   sizeof(readback)))
			return -EIO;
	}
	return 0;
}

static int gc573_edid_find_physical_address(u8 *offset)
{
	const u8 *edid = gc573_it6805_captured_edid;
	unsigned int data_end;
	unsigned int pos;

	if (!offset || edid[128] != 0x02 || edid[129] != 0x03 ||
	    edid[130] <= 4)
		return -EINVAL;

	data_end = 128 + edid[130];
	if (data_end > 255)
		return -EINVAL;

	for (pos = 132; pos < data_end; ) {
		unsigned int length = edid[pos] & 0x1f;
		unsigned int next = pos + length + 1;

		if (next > data_end)
			return -EINVAL;
		if ((edid[pos] >> 5) == 0x03 && length >= 5 &&
		    edid[pos + 1] == 0x03 && edid[pos + 2] == 0x0c &&
		    edid[pos + 3] == 0x00) {
			if (pos + 6 > data_end)
				return -EINVAL;
			*offset = pos + 4;
			return 0;
		}
		pos = next;
	}
	return -EINVAL;
}

static int gc573_edid_write_port_fields(struct gc573_device *dev, u8 bank,
					 u8 port_id, u8 checksum)
{
	int ret;

	ret = gc573_edid_set_bank(dev, bank);
	if (ret)
		return ret;
	ret = gc573_i2c_write_reg(dev, GC573_IT6805_ADDRESS, 0xc7, port_id);
	if (ret)
		return ret;
	ret = gc573_i2c_write_reg(dev, GC573_IT6805_ADDRESS, 0xc8, 0x00);
	if (ret)
		return ret;
	return gc573_i2c_write_reg(dev, GC573_IT6805_ADDRESS, 0xca, checksum);
}

static int gc573_edid_final_pulse(struct gc573_device *dev, u8 bank)
{
	u8 value;
	int ret;

	ret = gc573_edid_set_bank(dev, bank);
	if (ret)
		return ret;
	ret = gc573_i2c_read_reg(dev, GC573_IT6805_ADDRESS, 0xc5, &value);
	if (ret)
		return ret;
	ret = gc573_i2c_write_reg(dev, GC573_IT6805_ADDRESS, 0xc5,
				  value | BIT(4));
	if (ret)
		return ret;
	usleep_range(1000, 2000);
	return gc573_i2c_write_reg(dev, GC573_IT6805_ADDRESS, 0xc5,
				   value & ~BIT(4));
}

int gc573_it6805_program_edid(struct gc573_device *dev)
{
	u8 block_checksum[GC573_IT6805_EDID_BLOCKS];
	u8 port_checksum[2];
	u8 physical_offset;
	unsigned int i;
	int ret;
	int cleanup_ret;

	if (!dev || !dev->pdev)
		return -EINVAL;
	if (!dev->i2c_bus.registered)
		return -ENODEV;

	mutex_lock(&dev->lock);
	ret = gc573_edid_set_bank(dev, GC573_IT6805_BANK_0);
	if (ret)
		goto cleanup;
	ret = gc573_i2c_write_reg(dev, GC573_IT6805_ADDRESS, 0x4b, 0xa9);
	if (ret)
		goto cleanup;

	ret = gc573_edid_write_block(dev, 0, &block_checksum[0]);
	if (ret)
		goto cleanup;
	ret = gc573_edid_set_bank(dev, GC573_IT6805_BANK_0);
	if (ret)
		goto cleanup;
	ret = gc573_i2c_write_reg(dev, GC573_IT6805_ADDRESS, 0xc9,
				  block_checksum[0]);
	if (ret)
		goto cleanup;
	ret = gc573_edid_set_bank(dev, GC573_IT6805_BANK_PORT1);
	if (ret)
		goto cleanup;
	ret = gc573_i2c_write_reg(dev, GC573_IT6805_ADDRESS, 0xc9,
				  block_checksum[0]);
	if (ret)
		goto cleanup;

	ret = gc573_edid_write_block(dev, 1, &block_checksum[1]);
	if (ret)
		goto cleanup;
	ret = gc573_edid_find_physical_address(&physical_offset);
	if (ret)
		goto cleanup;
	port_checksum[0] = block_checksum[1] +
		gc573_it6805_captured_edid[physical_offset] +
		gc573_it6805_captured_edid[physical_offset + 1] - 0x10;
	port_checksum[1] = block_checksum[1] +
		gc573_it6805_captured_edid[physical_offset] +
		gc573_it6805_captured_edid[physical_offset + 1] - 0x20;

	ret = gc573_edid_set_bank(dev, GC573_IT6805_BANK_0);
	if (ret)
		goto cleanup;
	ret = gc573_i2c_write_reg(dev, GC573_IT6805_ADDRESS, 0xc6,
				  physical_offset);
	if (ret)
		goto cleanup;
	ret = gc573_edid_write_port_fields(dev, GC573_IT6805_BANK_0,
					   0x10, port_checksum[0]);
	if (ret)
		goto cleanup;
	ret = gc573_edid_write_port_fields(dev, GC573_IT6805_BANK_PORT1,
					   0x20, port_checksum[1]);
	if (ret)
		goto cleanup;

	ret = gc573_edid_verify(dev);
	if (ret)
		goto cleanup;

	/* Match the reference: clear update bits, then pulse EDID update per port. */
	for (i = 0; i < 2; i++) {
		u8 bank = i ? GC573_IT6805_BANK_PORT1 : GC573_IT6805_BANK_0;
		u8 value;

		ret = gc573_edid_set_bank(dev, bank);
		if (ret)
			goto cleanup;
		ret = gc573_i2c_read_reg(dev, GC573_IT6805_ADDRESS, 0xc5,
					 &value);
		if (ret)
			goto cleanup;
		ret = gc573_i2c_write_reg(dev, GC573_IT6805_ADDRESS, 0xc5,
					  value & ~BIT(0));
		if (ret)
			goto cleanup;
	}
	ret = gc573_edid_final_pulse(dev, GC573_IT6805_BANK_0);
	if (ret)
		goto cleanup;
	ret = gc573_edid_final_pulse(dev, GC573_IT6805_BANK_PORT1);

cleanup:
	cleanup_ret = gc573_edid_set_bank(dev, GC573_IT6805_BANK_0);
	if (!ret)
		ret = cleanup_ret;
	mutex_unlock(&dev->lock);
	if (ret)
		return ret;

	dev_info(&dev->pdev->dev,
		 "programmed captured IT6805 EDID: checksums=%02x/%02x physical-offset=%02x port-checksums=%02x/%02x\n",
		 block_checksum[0], block_checksum[1], physical_offset,
		 port_checksum[0], port_checksum[1]);
	return 0;
}
