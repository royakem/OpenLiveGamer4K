// SPDX-License-Identifier: GPL-2.0-only
/*
 * Bounded, selected-port IT6664 TX setup.
 *
 * Local port preparation and the bounded TX1 native path are ported from
 * ostrich/gc555: gc555-it6664-tx.c.
 * The native path supports exact checked progressive CTA timings from the
 * centralized mode table, plus the two listed 1280x800 DMT timings, as RGB8
 * on TX1 (board address 0x6a). CTA timings must also be present in sink EDID.
 * Shared switch/common initialization, RX/EDID/
 * HPD ownership, converters, scaler, audio, HDCP, and runtime work are outside
 * this file's closure. TX1 port-enable and direct-source route fields are the
 * only shared switch bits touched, with read/modify/write masks.
 *
 * Board I2C helpers use even 8-bit logical addresses. TX port N is addressed
 * at 0x68 + 2*N; the common-map signature is checked before any write.
 */
#include <linux/errno.h>
#include <linux/delay.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/math64.h>
#include <linux/hdmi.h>
#include <linux/types.h>

#include "gc573_bridge_tx.h"
#include "gc573_edid.h"
#include "gc573_input_modes.h"
#include "gc573_color.h"
#include "gc573_pure.h"

#define GC573_IT6664_TX_COMMON_ADDRESS	0x96
#define GC573_IT6664_TX_PORT0_ADDRESS	0x68
#define GC573_IT6664_TX_PORT_COUNT	4
#define GC573_IT6664_TX1_ADDRESS	0x6a
#define GC573_IT6664_RX_ADDRESS	0x70
#define GC573_IT6664_SWITCH_ADDRESS	0x58
#define GC573_TX_STATUS			0x03
#define GC573_TX_DDC_ENABLE		0x28
#define GC573_TX_DDC_SLAVE		0x29
#define GC573_TX_DDC_OFFSET		0x2a
#define GC573_TX_DDC_COUNT		0x2b
#define GC573_TX_DDC_HEADER		0x2c
#define GC573_TX_DDC_SEGMENT		0x2d
#define GC573_TX_DDC_COMMAND		0x2e
#define GC573_TX_DDC_STATUS		0x2f
#define GC573_TX_DDC_FIFO		0x30
#define GC573_EDID_SIZE			256
#define GC573_EDID_BLOCK_SIZE		128
#define GC573_EDID_CHUNK_SIZE		32
#define GC573_DDC_POLLS			80
#define GC573_RX_SAMPLES		100
#define GC573_RX_COUNTER_SCALE		0xc800
#define GC573_TX_PCLK_SAMPLES		10
#define GC573_TX1_RX_LOCK_TIMEOUT_MS	10000
#define GC573_TX1_RX_LOCK_STABLE_MS	1000
#define GC573_TX1_RX_LOCK_POLL_MS	20

static const u8 gc573_it6664_tx_common_id[] = {
	0x54, 0x49, 0x64, 0x66,
};

static u8 gc573_it6664_tx_port_address(unsigned int port)
{
	return GC573_IT6664_TX_PORT0_ADDRESS + 2 * port;
}

static int gc573_it6664_tx_read(struct gc573_device *dev, u8 address,
				u8 reg, u8 *value)
{
	return gc573_i2c_read_reg(dev, address, reg, value);
}

static int gc573_it6664_tx_write(struct gc573_device *dev, u8 address,
				 u8 reg, u8 value)
{
	return gc573_i2c_write_reg(dev, address, reg, value);
}

static int gc573_it6664_tx_update_bits(struct gc573_device *dev, u8 address,
				       u8 reg, u8 mask, u8 value)
{
	u8 old_value;
	int ret;

	ret = gc573_it6664_tx_read(dev, address, reg, &old_value);
	if (ret)
		return ret;

	return gc573_it6664_tx_write(dev, address, reg,
				     (old_value & ~mask) | (value & mask));
}

static int gc573_it6664_tx_check_common_id(struct gc573_device *dev)
{
	u8 id[ARRAY_SIZE(gc573_it6664_tx_common_id)];
	unsigned int i;
	int ret;

	for (i = 0; i < ARRAY_SIZE(id); i++) {
		ret = gc573_it6664_tx_read(dev,
					   GC573_IT6664_TX_COMMON_ADDRESS, i,
					   &id[i]);
		if (ret)
			return ret;
	}

	for (i = 0; i < ARRAY_SIZE(id); i++) {
		if (id[i] != gc573_it6664_tx_common_id[i])
			return -ENODEV;
	}

	return 0;
}

static int gc573_it6664_initialize_tx_port(struct gc573_device *dev,
					   u8 address)
{
	int ret;

	ret = gc573_it6664_tx_update_bits(dev, address, 0x08, 0x1c, 0);
	if (ret)
		return ret;
	ret = gc573_it6664_tx_update_bits(dev, address, 0x02, BIT(1), BIT(1));
	if (ret)
		return ret;
	ret = gc573_it6664_tx_update_bits(dev, address, 0x41, BIT(0), 0);
	if (ret)
		return ret;
	ret = gc573_it6664_tx_update_bits(dev, address, 0xc0, BIT(0), BIT(0));
	if (ret)
		return ret;
	ret = gc573_it6664_tx_update_bits(dev, address, 0x34, 0xc0, 0x80);
	if (ret)
		return ret;
	ret = gc573_it6664_tx_update_bits(dev, address, 0x35, 0x03, 0);
	if (ret)
		return ret;
	ret = gc573_it6664_tx_update_bits(dev, address, 0x3a, 0xfc, 0x90);
	if (ret)
		return ret;
	ret = gc573_it6664_tx_update_bits(dev, address, 0x93, 0xff, 0x40);
	if (ret)
		return ret;
	ret = gc573_it6664_tx_update_bits(dev, address, 0x94, 0x3e, 0x26);
	if (ret)
		return ret;
	ret = gc573_it6664_tx_update_bits(dev, address, 0xc0, BIT(4), BIT(4));
	if (ret)
		return ret;
	ret = gc573_it6664_tx_update_bits(dev, address, 0xc1, BIT(2), 0);
	if (ret)
		return ret;
	ret = gc573_it6664_tx_update_bits(dev, address, 0xc3, 0x0f, 0x01);
	if (ret)
		return ret;
	ret = gc573_it6664_tx_update_bits(dev, address, 0x18, 0x03, 0x03);
	if (ret)
		return ret;
	ret = gc573_it6664_tx_write(dev, address, 0x19, 0x07);
	if (ret)
		return ret;
	ret = gc573_it6664_tx_write(dev, address, 0x1a, 0x03);
	if (ret)
		return ret;
	ret = gc573_it6664_tx_write(dev, address, 0x1b, 0xff);
	if (ret)
		return ret;
	ret = gc573_it6664_tx_write(dev, address, 0x1c, 0x03);
	if (ret)
		return ret;
	ret = gc573_it6664_tx_write(dev, address, 0x88, 0x54);
	if (ret)
		return ret;
	ret = gc573_it6664_tx_write(dev, address, 0x8a, 0x00);
	if (ret)
		return ret;

	return gc573_it6664_tx_write(dev, address, 0x8b, 0x07);
}

static int gc573_it6664_reset_tx_video_clock(struct gc573_device *dev,
					     u8 address)
{
	int ret;

	ret = gc573_it6664_tx_write(dev, address, 0x01, 0x06);
	if (ret)
		return ret;
	ret = gc573_it6664_tx_update_bits(dev, address, 0x94, BIT(0), BIT(0));
	if (ret)
		return ret;
	ret = gc573_it6664_tx_update_bits(dev, address, 0x94, BIT(0), 0);
	if (ret)
		return ret;
	ret = gc573_it6664_tx_write(dev, address, 0x01, 0x04);
	if (ret)
		return ret;

	return gc573_it6664_tx_write(dev, address, 0x01, 0x00);
}

/* Local part of it6664_reset_tx_port(); switch 0x0c pulse is omitted. */
static int gc573_it6664_reset_tx_port_local(struct gc573_device *dev,
					    u8 address)
{
	int ret;

	ret = gc573_it6664_reset_tx_video_clock(dev, address);
	if (ret)
		return ret;
	ret = gc573_it6664_tx_update_bits(dev, address, 0x01, BIT(5), BIT(5));
	if (ret)
		return ret;
	ret = gc573_it6664_tx_update_bits(dev, address, 0x01, BIT(5), 0);
	if (ret)
		return ret;
	ret = gc573_it6664_tx_update_bits(dev, address, 0x18, BIT(7), 0);
	if (ret)
		return ret;
	ret = gc573_it6664_tx_write(dev, address, 0x19, 0x00);
	if (ret)
		return ret;
	ret = gc573_it6664_tx_write(dev, address, 0x1a, 0x00);
	if (ret)
		return ret;
	ret = gc573_it6664_tx_write(dev, address, 0x1b, 0x00);
	if (ret)
		return ret;
	ret = gc573_it6664_tx_write(dev, address, 0x1c, 0x00);
	if (ret)
		return ret;
	ret = gc573_it6664_tx_update_bits(dev, address, 0x35, BIT(4), BIT(4));
	if (ret)
		return ret;

	return gc573_it6664_tx_update_bits(dev, address, 0x35, BIT(4), 0);
}

/* Local-register subset of it6664_power_down_tx_port(). */
static int gc573_it6664_power_down_tx_port_local(struct gc573_device *dev,
						 u8 address)
{
	int ret;

	ret = gc573_it6664_tx_update_bits(dev, address, 0x18, 0xdc, 0);
	if (ret)
		return ret;
	ret = gc573_it6664_tx_update_bits(dev, address, 0x19, 0x07, 0);
	if (ret)
		return ret;
	ret = gc573_it6664_tx_update_bits(dev, address, 0x1a, 0xff, 0);
	if (ret)
		return ret;
	ret = gc573_it6664_tx_update_bits(dev, address, 0x1b, 0xff, 0);
	if (ret)
		return ret;
	ret = gc573_it6664_tx_update_bits(dev, address, 0x1c, 0xff, 0);
	if (ret)
		return ret;
	ret = gc573_it6664_tx_update_bits(dev, address, 0x84, 0x60, 0x60);
	if (ret)
		return ret;
	ret = gc573_it6664_tx_update_bits(dev, address, 0x86, BIT(3), 0);
	if (ret)
		return ret;
	ret = gc573_it6664_tx_update_bits(dev, address, 0x88, 0x03, 0x03);
	if (ret)
		return ret;

	return gc573_it6664_tx_update_bits(dev, address, 0x01, 0x26, 0x26);
}

int gc573_bridge_tx_prepare(struct gc573_device *dev, unsigned int port)
{
	u8 address;
	int ret;

	if (!dev)
		return -EINVAL;
	if (port >= GC573_IT6664_TX_PORT_COUNT)
		return -EINVAL;

	ret = gc573_it6664_tx_check_common_id(dev);
	if (ret)
		return ret;

	address = gc573_it6664_tx_port_address(port);
	ret = gc573_it6664_initialize_tx_port(dev, address);
	if (ret)
		return ret;
	ret = gc573_it6664_reset_tx_port_local(dev, address);
	if (ret)
		return ret;
	ret = gc573_it6664_power_down_tx_port_local(dev, address);
	if (ret)
		return ret;

	return gc573_it6664_reset_tx_port_local(dev, address);
}

static int gc573_tx1_edid_wait(struct gc573_device *dev)
{
	u8 status;
	unsigned int poll;
	int ret;

	/* Allow the command to replace the previous completion status. */
	usleep_range(15000, 16000);
	for (poll = 0; poll < GC573_DDC_POLLS; poll++) {
		ret = gc573_it6664_tx_read(dev, GC573_IT6664_TX1_ADDRESS,
					   GC573_TX_DDC_STATUS, &status);
		if (ret)
			return ret;
		if (status == 0xff)
			return -ENODEV;
		if (status & BIT(7))
			return status & 0x38 ? -EIO : 0;
		usleep_range(1000, 2000);
	}

	return -ETIMEDOUT;
}

static int gc573_tx1_read_edid_chunk(struct gc573_device *dev,
				     unsigned int block, unsigned int offset,
				     u8 *data)
{
	u8 status;
	int cleanup_ret;
	int ret;

	ret = gc573_it6664_tx_update_bits(dev, GC573_IT6664_TX1_ADDRESS,
					 GC573_TX_DDC_ENABLE, BIT(0), BIT(0));
	if (ret)
		goto cleanup;
	ret = gc573_it6664_tx_update_bits(dev, GC573_IT6664_TX1_ADDRESS,
					 0x19, BIT(2), BIT(2));
	if (ret)
		goto cleanup;
	ret = gc573_it6664_tx_update_bits(dev, GC573_IT6664_TX1_ADDRESS,
					 0x1d, BIT(3), BIT(3));
	if (ret)
		goto cleanup;
	ret = gc573_it6664_tx_write(dev, GC573_IT6664_TX1_ADDRESS,
				    GC573_TX_DDC_COMMAND, 0x09);
	if (ret)
		goto cleanup;
	ret = gc573_it6664_tx_write(dev, GC573_IT6664_TX1_ADDRESS,
				    GC573_TX_DDC_SLAVE, 0xa0);
	if (ret)
		goto cleanup;
	ret = gc573_it6664_tx_write(dev, GC573_IT6664_TX1_ADDRESS,
				    GC573_TX_DDC_OFFSET,
				    offset);
	if (ret)
		goto cleanup;
	ret = gc573_it6664_tx_write(dev, GC573_IT6664_TX1_ADDRESS,
				    GC573_TX_DDC_COUNT, GC573_EDID_CHUNK_SIZE);
	if (ret)
		goto cleanup;
	ret = gc573_it6664_tx_write(dev, GC573_IT6664_TX1_ADDRESS,
				    GC573_TX_DDC_HEADER, 0);
	if (ret)
		goto cleanup;
	ret = gc573_it6664_tx_write(dev, GC573_IT6664_TX1_ADDRESS,
				    GC573_TX_DDC_SEGMENT, block >> 1);
	if (ret)
		goto cleanup;
	ret = gc573_it6664_tx_read(dev, GC573_IT6664_TX1_ADDRESS,
				   GC573_TX_STATUS, &status);
	if (ret)
		goto cleanup;
	if (status == 0xff || !(status & BIT(0))) {
		ret = -ENOLINK;
		goto cleanup;
	}
	ret = gc573_it6664_tx_write(dev, GC573_IT6664_TX1_ADDRESS,
				    GC573_TX_DDC_COMMAND, 0x03);
	if (ret)
		goto cleanup;
	ret = gc573_tx1_edid_wait(dev);
	if (ret)
		goto cleanup;
	ret = gc573_i2c_read_block(dev, GC573_IT6664_TX1_ADDRESS,
				   GC573_TX_DDC_FIFO, data,
				   GC573_EDID_CHUNK_SIZE);

cleanup:
	cleanup_ret = gc573_it6664_tx_update_bits(dev,
			GC573_IT6664_TX1_ADDRESS, GC573_TX_DDC_ENABLE,
			BIT(0), 0);
	return ret ? ret : cleanup_ret;
}

static int gc573_tx1_read_edid(struct gc573_device *dev, u8 *edid)
{
	unsigned int block, offset, i;
	int ret;

	for (block = 0; block < 2; block++) {
		for (offset = 0; offset < GC573_EDID_BLOCK_SIZE;
		     offset += GC573_EDID_CHUNK_SIZE) {
			ret = gc573_tx1_read_edid_chunk(dev, block,
						block * GC573_EDID_BLOCK_SIZE + offset,
						edid + block * GC573_EDID_BLOCK_SIZE +
						offset);
			if (ret)
				return ret;
		}
		if (edid[block * GC573_EDID_BLOCK_SIZE] == 0xff)
			return -EBADMSG;
		{
			u8 sum = 0;

			for (i = 0; i < GC573_EDID_BLOCK_SIZE; i++)
				sum += edid[block * GC573_EDID_BLOCK_SIZE + i];
			if (sum)
				return -EBADMSG;
		}
	}
	if (edid[126] != 1 || edid[128] != 0x02) {
		dev_err(&dev->pdev->dev, "TX1 EDID extension count=%u tag=%02x\n",
			edid[126], edid[128]);
		return -EOPNOTSUPP;
	}
	return 0;
}

static int gc573_tx1_validate_edid(struct gc573_device *dev,
                                   const struct gc573_input_mode *mode)
{
	u8 actual[GC573_EDID_SIZE];
	u8 expected[GC573_EDID_SIZE];
	unsigned int i, pos, end;
	bool requested_vic_found = false;
	bool hf_scdc = false;
	int ret;

	if (!mode)
		return -EINVAL;
	ret = gc573_it6805_get_edid(expected, sizeof(expected));
	if (ret)
		return ret;
	ret = gc573_tx1_read_edid(dev, actual);
	if (ret)
		return ret;
	/* Preserve the byte-for-byte EDID identity check; RAM owns checksums. */
	for (i = 0; i < GC573_EDID_SIZE; i++) {
		if (i == 127 || i == 255)
			continue;
		if (actual[i] != expected[i]) {
			dev_err(&dev->pdev->dev,
				"TX1 EDID mismatch at %u: %02x expected %02x\n",
				i, actual[i], expected[i]);
			return -EOPNOTSUPP;
		}
	}
	end = actual[130];
	if (end < 4 || end > 127)
		return -EBADMSG;
	for (pos = 132; pos < 128 + end;) {
		u8 header = actual[pos++];
		u8 tag = header >> 5;
		u8 length = header & 0x1f;
		unsigned int j;

		if (pos + length > 128 + end)
			return -EBADMSG;
		if (tag == 2) {
			for (j = 0; j < length; j++) {
				if ((actual[pos + j] & 0x7f) == mode->vic)
					requested_vic_found = true;
			}
		} else if (tag == 3 && length >= 6 &&
			   actual[pos] == 0xd8 && actual[pos + 1] == 0x5d &&
			   actual[pos + 2] == 0xc4 && actual[pos + 4] >= 119 &&
			   (actual[pos + 5] & BIT(7))) {
			hf_scdc = true;
		}
		pos += length;
	}
	if (!mode->vic)
		return 0;
	if (!requested_vic_found)
		return -EOPNOTSUPP;
	if (mode->vic >= 96 && !hf_scdc)
		return -EOPNOTSUPP;
	return 0;
}

static int gc573_tx1_validate_rx(struct gc573_device *dev, u32 *pixel_khz)
{
	u8 value, avi, quantization, depth_raw, rx_status, count_hi, count_lo, dummy;
	u8 avi_packet[18] = { 0 };
	u16 hactive, vactive, htotal, vtotal;
	u8 vic;
	bool rgb, depth_8bit, rgb_limited;
	u32 sum = 0, rclk_khz, measured_khz;
	const struct gc573_input_mode *mode;
	unsigned int i;
	int ret, cleanup_ret;

	ret = gc573_it6664_tx_update_bits(dev, GC573_IT6664_RX_ADDRESS,
					 0x0f, 0x03, 0);
	if (ret)
		return ret;
	ret = gc573_it6664_tx_read(dev, GC573_IT6664_RX_ADDRESS, 0x0f, &value);
	if (ret)
		return ret;
	if (value == 0xff)
		return -ENODEV;
	ret = gc573_it6664_tx_read(dev, GC573_IT6664_RX_ADDRESS, 0x13,
				   &rx_status);
	if (ret)
		return ret;
	if (rx_status == 0xff)
		return -ENODEV;
	if (!(rx_status & BIT(1)))
		return -EOPNOTSUPP;
	ret = gc573_it6664_tx_write(dev, GC573_IT6664_RX_ADDRESS, 0x0f,
				    (value & ~0x03) | 0x02);
	if (ret)
		return ret;
	ret = gc573_i2c_read_block(dev, GC573_IT6664_RX_ADDRESS, 0x10,
				   avi_packet, sizeof(avi_packet));
	cleanup_ret = gc573_it6664_tx_update_bits(dev, GC573_IT6664_RX_ADDRESS,
						 0x0f, 0x03, 0);
	if (ret)
		return ret;
	if (cleanup_ret)
		return cleanup_ret;
	{
		u8 checksum = HDMI_INFOFRAME_TYPE_AVI + avi_packet[2] +
			avi_packet[3];

		for (i = 4; i < sizeof(avi_packet); i++)
			checksum += avi_packet[i];
		if (avi_packet[2] != HDMI_AVI_INFOFRAME_SIZE ||
		    avi_packet[3] != 2 || checksum)
			return -EBADMSG;
		avi = avi_packet[5];
		quantization = (avi_packet[7] >> 2) & 0x03;
		vic = avi_packet[8] & 0x7f;
		rgb = !(avi & 0x60) && !(avi_packet[9] & 0x0f);
	}

	ret = gc573_it6664_tx_read(dev, GC573_IT6664_RX_ADDRESS, 0x98,
				   &value);
	if (ret)
		return ret;
	if (value == 0xff)
		return -ENODEV;
	depth_raw = value;
	depth_8bit = ((depth_raw >> 4) & 0x03) == 0;
	ret = gc573_it6664_tx_read(dev, GC573_IT6664_RX_ADDRESS, 0x9e,
				   &count_hi);
	if (ret)
		return ret;
	ret = gc573_it6664_tx_read(dev, GC573_IT6664_RX_ADDRESS, 0x9d,
				   &count_lo);
	if (ret)
		return ret;
	if (count_hi == 0xff && count_lo == 0xff)
		return -ENODEV;
	hactive = (((u16)count_hi << 8) | count_lo) & 0x3fff;
	ret = gc573_it6664_tx_read(dev, GC573_IT6664_RX_ADDRESS, 0xa5,
				   &count_hi);
	if (ret)
		return ret;
	ret = gc573_it6664_tx_read(dev, GC573_IT6664_RX_ADDRESS, 0xa4,
				   &count_lo);
	if (ret)
		return ret;
	if (count_hi == 0xff && count_lo == 0xff)
		return -ENODEV;
	vactive = (((u16)count_hi << 8) | count_lo) & 0x3fff;
	ret = gc573_it6664_tx_read(dev, GC573_IT6664_RX_ADDRESS, 0x9c,
				   &count_hi);
	if (ret)
		return ret;
	ret = gc573_it6664_tx_read(dev, GC573_IT6664_RX_ADDRESS, 0x9b,
				   &count_lo);
	if (ret)
		return ret;
	if (count_hi == 0xff && count_lo == 0xff)
		return -ENODEV;
	htotal = (((u16)count_hi << 8) | count_lo) & 0x3fff;
	ret = gc573_it6664_tx_read(dev, GC573_IT6664_RX_ADDRESS, 0xa3,
				   &count_hi);
	if (ret)
		return ret;
	ret = gc573_it6664_tx_read(dev, GC573_IT6664_RX_ADDRESS, 0xa2,
				   &count_lo);
	if (ret)
		return ret;
	if (count_hi == 0xff && count_lo == 0xff)
		return -ENODEV;
	vtotal = (((u16)count_hi << 8) | count_lo) & 0x3fff;

	ret = gc573_it6664_tx_read(dev, GC573_IT6664_SWITCH_ADDRESS, 0x1e,
				   &count_hi);
	if (ret)
		return ret;
	ret = gc573_it6664_tx_read(dev, GC573_IT6664_SWITCH_ADDRESS, 0x1f,
				   &count_lo);
	if (ret)
		return ret;
	rclk_khz = (count_hi & 0x3f) * 1000 + count_lo * 1000 / 256;
	if (rclk_khz < 10000 || rclk_khz > 40000)
		return -ERANGE;
	ret = gc573_it6664_tx_update_bits(dev, GC573_IT6664_RX_ADDRESS,
					 0x0f, 0x03, 0);
	if (ret)
		return ret;
	for (i = 0; i < GC573_RX_SAMPLES; i++) {
		ret = gc573_it6664_tx_read(dev, GC573_IT6664_RX_ADDRESS, 0x48,
					   &dummy);
		if (ret)
			return ret;
		if (dummy == 0xff)
			return -ENODEV;
	}
	ret = gc573_it6664_tx_read(dev, GC573_IT6664_RX_ADDRESS, 0x43, &dummy);
	if (ret)
		return ret;
	for (i = 0; i < GC573_RX_SAMPLES; i++) {
		ret = gc573_it6664_tx_read(dev, GC573_IT6664_RX_ADDRESS, 0x9a,
					   &count_hi);
		if (ret)
			return ret;
		ret = gc573_it6664_tx_read(dev, GC573_IT6664_RX_ADDRESS, 0x99,
					   &count_lo);
		if (ret)
			return ret;
		if (count_hi == 0xff && count_lo == 0xff)
			return -ENODEV;
		sum += (((u32)count_hi << 8) | count_lo) & 0x3ff;
	}
	if (!sum)
		return -ERANGE;
	measured_khz = div_u64((u64)rclk_khz * GC573_RX_COUNTER_SCALE, sum);
	dev_info(&dev->pdev->dev,
		 "IT6664 RX mode diagnostic AVI=%*ph depth-reg=%02x RGB=%u raster=%ux%u total=%ux%u clock-counter=%u kHz\n",
		 (int)sizeof(avi_packet), avi_packet, depth_raw, rgb,
		 hactive, vactive, htotal, vtotal, measured_khz);

	if (!rgb || !depth_8bit)
		return -EOPNOTSUPP;
	mode = gc573_input_mode_find(vic, hactive, vactive, htotal, vtotal);
	if (!mode || !gc573_input_mode_clock_valid(mode, measured_khz))
		return -EOPNOTSUPP;
	ret = gc573_rgb_quantization_resolve(quantization, mode->vic,
					       &rgb_limited);
	if (ret)
		return ret;

	dev->input_width = hactive;
	dev->input_height = vactive;
	dev->input_htotal = htotal;
	dev->input_vtotal = vtotal;
	dev->input_vic = vic;
	dev->input_rgb_limited = rgb_limited;
	dev_info(&dev->pdev->dev,
		 "IT6664 RX checked RGB8 AVI-VIC%u mode-VIC%u range=%s raster=%ux%u total=%ux%u\n",
		 vic, mode->vic, rgb_limited ? "limited" : "full",
		 hactive, vactive, htotal, vtotal);
	*pixel_khz = measured_khz;
	return 0;
}

int gc573_bridge_validate_native(struct gc573_device *dev)
{
	u32 pixel_khz;

	if (!dev)
		return -EINVAL;
	return gc573_tx1_validate_rx(dev, &pixel_khz);
}

static int gc573_tx1_measure_clock(struct gc573_device *dev, u32 *pixel_khz)
{
	u32 initial, sum = 0, divider, average;
	u8 lo, hi;
	unsigned int i;
	int ret;

	ret = gc573_it6664_tx_update_bits(dev, GC573_IT6664_TX1_ADDRESS,
					 0x07, BIT(7), BIT(7));
	if (ret)
		return ret;
	usleep_range(1000, 2000);
	ret = gc573_it6664_tx_update_bits(dev, GC573_IT6664_TX1_ADDRESS,
					 0x07, BIT(7), 0);
	if (ret)
		return ret;
	ret = gc573_it6664_tx_read(dev, GC573_IT6664_TX1_ADDRESS, 0x06, &lo);
	if (ret)
		return ret;
	ret = gc573_it6664_tx_read(dev, GC573_IT6664_TX1_ADDRESS, 0x07, &hi);
	if (ret)
		return ret;
	initial = ((hi & 0x0f) << 9) | (lo << 1);
	if (!initial || (hi == 0xff && lo == 0xff))
		return -ENOLINK;
	divider = initial < 0x10 ? 7 : initial < 0x20 ? 6 :
		  initial < 0x40 ? 5 : initial < 0x80 ? 4 :
		  initial < 0x100 ? 3 : initial < 0x200 ? 2 :
		  initial < 0x400 ? 1 : 0;
	for (i = 0; i < GC573_TX_PCLK_SAMPLES; i++) {
		u8 selector = divider << 4;

		ret = gc573_it6664_tx_update_bits(dev, GC573_IT6664_TX1_ADDRESS,
						 0x07, 0xf0, selector | BIT(7));
		if (ret)
			return ret;
		ret = gc573_it6664_tx_update_bits(dev, GC573_IT6664_TX1_ADDRESS,
						 0x07, 0xf0, selector);
		if (ret)
			return ret;
		usleep_range(1000, 2000);
		ret = gc573_it6664_tx_read(dev, GC573_IT6664_TX1_ADDRESS, 0x06,
					   &lo);
		if (ret)
			return ret;
		ret = gc573_it6664_tx_read(dev, GC573_IT6664_TX1_ADDRESS, 0x07,
					   &hi);
		if (ret)
			return ret;
		if (hi == 0xff && lo == 0xff)
			return -ENODEV;
		sum += ((hi & 0x0f) << 9) | (lo << 1);
	}
	average = max_t(u32, sum / (GC573_TX_PCLK_SAMPLES << divider), 1);
	{
		u8 integer, fraction;
		u32 rclk;

		ret = gc573_it6664_tx_read(dev, GC573_IT6664_SWITCH_ADDRESS,
					   0x1e, &integer);
		if (ret)
			return ret;
		ret = gc573_it6664_tx_read(dev, GC573_IT6664_SWITCH_ADDRESS,
					   0x1f, &fraction);
		if (ret)
			return ret;
		rclk = (integer & 0x3f) * 1000 + fraction * 1000 / 256;
		if (rclk < 10000 || rclk > 40000)
			return -ERANGE;
		*pixel_khz = div_u64((u64)rclk << 12, average);
	}
	dev_info(&dev->pdev->dev, "IT6664 TX1 raw initial=%u divider=%u sum=%u average=%u clock=%u kHz\n",
		 initial, divider, sum, average, *pixel_khz);
	if (*pixel_khz < 25000 || *pixel_khz >= 621000)
		return -EOPNOTSUPP;
	return 0;
}

static int gc573_tx1_scdc_transfer(struct gc573_device *dev, bool write,
				   u8 offset, u8 *value)
{
	u8 status;
	int cleanup_ret, ret;

	if (!value)
		return -EINVAL;
	ret = gc573_it6664_tx_read(dev, GC573_IT6664_TX1_ADDRESS,
				   GC573_TX_STATUS, &status);
	if (ret)
		goto cleanup;
	if (status == 0xff || !(status & BIT(0))) {
		ret = -ENOLINK;
		goto cleanup;
	}
	ret = gc573_it6664_tx_update_bits(dev, GC573_IT6664_TX1_ADDRESS,
					 GC573_TX_DDC_ENABLE, BIT(0), 0);
	if (ret)
		goto cleanup;
	ret = gc573_it6664_tx_write(dev, GC573_IT6664_TX1_ADDRESS,
				    GC573_TX_DDC_COMMAND, 0x09);
	if (ret)
		goto cleanup;
	ret = gc573_it6664_tx_write(dev, GC573_IT6664_TX1_ADDRESS,
				    GC573_TX_DDC_SLAVE, 0xa8);
	if (ret)
		goto cleanup;
	ret = gc573_it6664_tx_write(dev, GC573_IT6664_TX1_ADDRESS,
				    GC573_TX_DDC_OFFSET, offset);
	if (ret)
		goto cleanup;
	ret = gc573_it6664_tx_write(dev, GC573_IT6664_TX1_ADDRESS,
				    GC573_TX_DDC_COUNT, 1);
	if (ret)
		goto cleanup;
	ret = gc573_it6664_tx_update_bits(dev, GC573_IT6664_TX1_ADDRESS,
					 GC573_TX_DDC_HEADER, 0x03, 0);
	if (ret)
		goto cleanup;
	if (write) {
		ret = gc573_it6664_tx_write(dev, GC573_IT6664_TX1_ADDRESS,
					    GC573_TX_DDC_FIFO, *value);
		if (ret)
			goto cleanup;
	}
	ret = gc573_it6664_tx_write(dev, GC573_IT6664_TX1_ADDRESS,
				    GC573_TX_DDC_COMMAND, write ? 0x01 : 0x00);
	if (ret)
		goto cleanup;
	ret = gc573_tx1_edid_wait(dev);
	if (!ret && !write)
		ret = gc573_it6664_tx_read(dev, GC573_IT6664_TX1_ADDRESS,
					   GC573_TX_DDC_FIFO, value);

cleanup:
	cleanup_ret = gc573_it6664_tx_update_bits(dev,
			GC573_IT6664_TX1_ADDRESS, GC573_TX_DDC_ENABLE,
			BIT(0), 0);
	return ret ? ret : cleanup_ret;
}

static int gc573_tx1_setup_scdc(struct gc573_device *dev)
{
	u8 value;
	int ret;

	ret = gc573_it6664_tx_update_bits(dev, GC573_IT6664_TX1_ADDRESS,
					 0x83, BIT(3), BIT(3));
	if (ret)
		return ret;
	ret = gc573_it6664_tx_update_bits(dev, GC573_IT6664_TX1_ADDRESS,
					 0xc0, 0x46, 0x46);
	if (ret)
		return ret;
	ret = gc573_it6664_tx_update_bits(dev, GC573_IT6664_TX1_ADDRESS,
					 0x3a, 0x03, 0);
	if (ret)
		return ret;
	value = 1;
	ret = gc573_tx1_scdc_transfer(dev, true, 0x02, &value);
	if (ret)
		return ret;
	value = 0;
	ret = gc573_tx1_scdc_transfer(dev, false, 0x02, &value);
	if (ret)
		return ret;
	if (value < 1)
		return -EOPNOTSUPP;
	value = 0x03;
	ret = gc573_tx1_scdc_transfer(dev, true, 0x20, &value);
	if (ret)
		return ret;
	value = 0;
	ret = gc573_tx1_scdc_transfer(dev, false, 0x20, &value);
	if (ret)
		return ret;
	return (value & 0x03) == 0x03 ? 0 : -EIO;
}

/* Low-bandwidth RGB8 timings do not require SCDC scrambling. */
static int gc573_tx1_clear_scdc(struct gc573_device *dev)
{
	u8 value = 0;
	int ret;

	ret = gc573_tx1_scdc_transfer(dev, true, 0x20, &value);
	if (ret)
		return ret;
	ret = gc573_tx1_scdc_transfer(dev, false, 0x20, &value);
	if (ret)
		return ret;
	if (value & 0x03)
		return -EIO;
	return gc573_it6664_tx_update_bits(dev, GC573_IT6664_TX1_ADDRESS,
					    0xc0, BIT(1), 0);
}

/* Fast mode-change guard: bank 2 contains AVI; always leave RX in bank 0. */
int gc573_bridge_check_mode(struct gc573_device *dev)
{
	u8 bank, status, packet[18] = { 0 };
	u16 width, height, htotal, vtotal;
	u8 vic, checksum, quantization;
	bool rgb_limited;
	const struct gc573_input_mode *mode;
	unsigned int i;
	int ret, cleanup;

	if (!dev)
		return -EINVAL;
	ret = gc573_it6664_tx_read(dev, GC573_IT6664_RX_ADDRESS, 0x0f, &bank);
	if (ret)
		return ret;
	if (bank == 0xff)
		return -ENODEV;
	ret = gc573_it6664_tx_update_bits(dev, GC573_IT6664_RX_ADDRESS,
					 0x0f, 0x03, 0);
	if (ret)
		goto restore_bank0;
	ret = gc573_it6664_tx_read(dev, GC573_IT6664_RX_ADDRESS, 0x13, &status);
	if (ret || status == 0xff || !(status & BIT(1))) {
		ret = ret ? ret : -EAGAIN;
		goto restore_bank0;
	}
	ret = gc573_it6664_tx_update_bits(dev, GC573_IT6664_RX_ADDRESS,
					 0x0f, 0x03, 0x02);
	if (ret)
		goto restore_bank0;
	ret = gc573_i2c_read_block(dev, GC573_IT6664_RX_ADDRESS, 0x10,
				   packet, sizeof(packet));
	cleanup = gc573_it6664_tx_update_bits(dev, GC573_IT6664_RX_ADDRESS,
						 0x0f, 0x03, 0);
	if (!ret)
		ret = cleanup;
	if (ret)
		goto restore_bank0;

	checksum = HDMI_INFOFRAME_TYPE_AVI + packet[2] + packet[3];
	for (i = 4; i < sizeof(packet); i++)
		checksum += packet[i];
	if (packet[2] != HDMI_AVI_INFOFRAME_SIZE || packet[3] != 2 ||
	    checksum || (packet[9] & 0x0f) || (packet[5] & 0x60)) {
		ret = -EAGAIN;
		goto restore_bank0;
	}
	vic = packet[8] & 0x7f;
	quantization = (packet[7] >> 2) & 0x03;
	ret = gc573_it6664_tx_read(dev, GC573_IT6664_RX_ADDRESS, 0x9e,
				   &status);
	if (ret)
		goto restore_bank0;
	width = (status << 8);
	ret = gc573_it6664_tx_read(dev, GC573_IT6664_RX_ADDRESS, 0x9d,
				   &status);
	if (ret)
		goto restore_bank0;
	width = (width | status) & 0x3fff;
	ret = gc573_it6664_tx_read(dev, GC573_IT6664_RX_ADDRESS, 0xa5,
				   &status);
	if (ret)
		goto restore_bank0;
	height = status << 8;
	ret = gc573_it6664_tx_read(dev, GC573_IT6664_RX_ADDRESS, 0xa4,
				   &status);
	if (ret)
		goto restore_bank0;
	height = (height | status) & 0x3fff;
	ret = gc573_it6664_tx_read(dev, GC573_IT6664_RX_ADDRESS, 0x9c,
				   &status);
	if (ret)
		goto restore_bank0;
	htotal = status << 8;
	ret = gc573_it6664_tx_read(dev, GC573_IT6664_RX_ADDRESS, 0x9b,
				   &status);
	if (ret)
		goto restore_bank0;
	htotal = (htotal | status) & 0x3fff;
	ret = gc573_it6664_tx_read(dev, GC573_IT6664_RX_ADDRESS, 0xa3,
				   &status);
	if (ret)
		goto restore_bank0;
	vtotal = status << 8;
	ret = gc573_it6664_tx_read(dev, GC573_IT6664_RX_ADDRESS, 0xa2,
				   &status);
	if (ret)
		goto restore_bank0;
	vtotal = (vtotal | status) & 0x3fff;
	ret = gc573_it6664_tx_read(dev, GC573_IT6664_RX_ADDRESS, 0x98, &status);
	if (ret)
		goto restore_bank0;
	if (status == 0xff || ((status >> 4) & 0x03)) {
		ret = -EAGAIN;
		goto restore_bank0;
	}
	mode = gc573_input_mode_find(vic, width, height, htotal, vtotal);
	if (mode && gc573_rgb_quantization_resolve(quantization, mode->vic,
						       &rgb_limited)) {
		ret = -EAGAIN;
		goto restore_bank0;
	}
	if (!mode || width != dev->input_width || height != dev->input_height ||
	    htotal != dev->input_htotal || vtotal != dev->input_vtotal ||
	    vic != dev->input_vic || rgb_limited != dev->input_rgb_limited)
		ret = -EAGAIN;

restore_bank0:
	cleanup = gc573_it6664_tx_update_bits(dev, GC573_IT6664_RX_ADDRESS,
					      0x0f, 0x03, 0);
	return ret ? ret : cleanup;
}

static int gc573_tx1_power_on(struct gc573_device *dev)
{
	int ret;

	/* Port 1 is board address 0x6a. Preserve the other three port bits. */
	ret = gc573_it6664_tx_update_bits(dev, GC573_IT6664_SWITCH_ADDRESS,
					 0x08, BIT(1), BIT(1));
	if (ret)
		return ret;
	ret = gc573_it6664_tx_update_bits(dev, GC573_IT6664_TX1_ADDRESS,
					 0xc1, 0xf0, 0x80);
	if (ret)
		return ret;
	ret = gc573_it6664_tx_update_bits(dev, GC573_IT6664_TX1_ADDRESS,
					 0x84, 0xe0, 0x80);
	if (ret)
		return ret;
	ret = gc573_it6664_tx_update_bits(dev, GC573_IT6664_TX1_ADDRESS,
					 0x86, BIT(3), BIT(3));
	if (ret)
		return ret;
	ret = gc573_it6664_tx_update_bits(dev, GC573_IT6664_TX1_ADDRESS,
					 0x02, BIT(0), 0);
	if (ret)
		return ret;
	ret = gc573_it6664_tx_update_bits(dev, GC573_IT6664_TX1_ADDRESS,
					 0x19, 0x07, 0x07);
	if (ret)
		return ret;
	msleep(100);
	return 0;
}

static int gc573_tx1_stage_error(struct gc573_device *dev, const char *stage,
				 int ret)
{
	if (ret)
		dev_err(&dev->pdev->dev, "IT6664 TX1 native %s failed: %d\n",
			stage, ret);
	return ret;
}

/*
 * TX1 output changes can make the downstream IT6805 reacquire even though
 * upstream IT6664 acquisition succeeded. Require its actual 5V+SCDT status
 * to remain locked for the receiver's existing 1 s settling interval, with
 * the same 20 ms polling cadence and 10 s bound as bridge acquisition.
 */
static int gc573_tx1_wait_receiver_stable(struct gc573_device *dev)
{
	unsigned long deadline, lock_since = 0;
	bool have_lock = false;
	int ret;

	if (!dev->it6805 || !dev->it6805->get_signal_status)
		return -EOPNOTSUPP;

	deadline = jiffies + msecs_to_jiffies(GC573_TX1_RX_LOCK_TIMEOUT_MS);
	for (;;) {
		bool locked;

		ret = dev->it6805->get_signal_status(dev, &locked);
		if (ret)
			return ret;

		if (locked) {
			if (!have_lock) {
				lock_since = jiffies;
				have_lock = true;
			}
			if (time_after_eq(jiffies,
					  lock_since +
					  msecs_to_jiffies(
						  GC573_TX1_RX_LOCK_STABLE_MS))) {
				dev_info(&dev->pdev->dev,
					 "IT6805 receiver lock stable for %u ms after TX1 enable\n",
					 GC573_TX1_RX_LOCK_STABLE_MS);
				return 0;
			}
		} else {
			have_lock = false;
		}

		if (time_after_eq(jiffies, deadline))
			break;
		msleep(GC573_TX1_RX_LOCK_POLL_MS);
	}

	dev_err(&dev->pdev->dev,
		"IT6805 receiver lock did not remain stable for %u ms within %u ms after TX1 enable\n",
		GC573_TX1_RX_LOCK_STABLE_MS, GC573_TX1_RX_LOCK_TIMEOUT_MS);
	return -ETIMEDOUT;
}

int gc573_bridge_tx_enable_native(struct gc573_device *dev)
{
	u8 status;
	u32 rx_clock, tx_clock;
	int ret;

	if (!dev || !dev->pdev)
		return -EINVAL;
	ret = gc573_it6664_tx_check_common_id(dev);
	if (ret)
		return ret;
	ret = gc573_it6664_tx_read(dev, GC573_IT6664_TX1_ADDRESS,
				   GC573_TX_STATUS, &status);
	if (ret)
		return ret;
	if (status == 0xff)
		return -ENODEV;
	if (!(status & BIT(0)))
		return -ENOLINK;
	ret = gc573_tx1_validate_rx(dev, &rx_clock);
	if (gc573_tx1_stage_error(dev, "RX format/timing validation", ret)) {
		/* Cross-check the independent TX counter without enabling output. */
		int diagnostic = gc573_tx1_measure_clock(dev, &tx_clock);
		dev_info(&dev->pdev->dev, "IT6664 TX1 diagnostic counter status=%d\n", diagnostic);
		return ret;
	}
	ret = gc573_tx1_validate_edid(dev,
		gc573_input_mode_find(dev->input_vic, dev->input_width,
			dev->input_height, dev->input_htotal, dev->input_vtotal));
	if (gc573_tx1_stage_error(dev, "sink EDID/capability validation", ret))
		return ret;
	ret = gc573_tx1_power_on(dev);
	if (gc573_tx1_stage_error(dev, "selected-port power-on", ret))
		return ret;
	ret = gc573_it6664_tx_update_bits(dev, GC573_IT6664_TX1_ADDRESS,
					 0xc0, BIT(0), BIT(0));
	if (ret)
		return gc573_tx1_stage_error(dev, "HDMI protocol selection", ret);

	/* Native direct RGB8 only. Measure the TX clock before setting its AFE. */
	ret = gc573_tx1_measure_clock(dev, &tx_clock);
	if (gc573_tx1_stage_error(dev, "TX pixel-clock measurement", ret))
		return ret;
	dev_info(&dev->pdev->dev,
		 "IT6664 TX1 measured source clocks RX/TX=%u/%u kHz\n",
		 rx_clock, tx_clock);
	if (abs((int)tx_clock - (int)rx_clock) > 12000) {
		dev_err(&dev->pdev->dev,
			"IT6664 TX1 source clock mismatch RX=%u TX=%u kHz\n",
		rx_clock, tx_clock);
		return -EOPNOTSUPP;
	}
	ret = gc573_it6664_tx_update_bits(dev, GC573_IT6664_TX1_ADDRESS,
					 0x84, 0x07,
					 tx_clock <= 100000 ? 0x03 : 0x04);
	if (ret)
		return ret;
	ret = gc573_it6664_tx_update_bits(dev, GC573_IT6664_TX1_ADDRESS,
					 0x88, BIT(2), tx_clock > 162000 ? BIT(2) : 0);
	if (ret)
		return ret;
	ret = gc573_it6664_tx_update_bits(dev, GC573_IT6664_TX1_ADDRESS,
					 0x87, 0x1f, tx_clock > 375000 ? 0x0e :
					 (tx_clock > 310000 ? 0x0d :
					  (tx_clock > 150000 ? 0x09 : 0x03)));
	if (ret)
		return ret;
	ret = gc573_it6664_tx_update_bits(dev, GC573_IT6664_TX1_ADDRESS,
					 0x89, 0xbf, tx_clock > 310000 ? 0x25 :
					 (tx_clock > 150000 ? 0x21 : 0x80));
	if (ret)
		return ret;
	ret = gc573_it6664_tx_update_bits(dev, GC573_IT6664_TX1_ADDRESS,
					 0x8a, 0x0f, 0);
	if (ret)
		return ret;
	ret = gc573_it6664_tx_update_bits(dev, GC573_IT6664_TX1_ADDRESS,
					 0x8b, 0x0f, tx_clock > 375000 ? 0x0d :
					 (tx_clock > 310000 ? 0x0b :
					  (tx_clock > 150000 ? 0x09 : 0x03)));
	if (ret)
		return ret;
	msleep(50);
	ret = gc573_it6664_tx_write(dev, GC573_IT6664_TX1_ADDRESS, 0x01, 0x06);
	if (ret)
		return ret;
	ret = gc573_it6664_tx_update_bits(dev, GC573_IT6664_TX1_ADDRESS,
					 0x94, BIT(0), BIT(0));
	if (ret)
		return ret;
	ret = gc573_it6664_tx_update_bits(dev, GC573_IT6664_TX1_ADDRESS,
					 0x94, BIT(0), 0);
	if (ret)
		return ret;
	ret = gc573_it6664_tx_write(dev, GC573_IT6664_TX1_ADDRESS, 0x01, 0x04);
	if (ret)
		return ret;
	ret = gc573_it6664_tx_write(dev, GC573_IT6664_TX1_ADDRESS, 0x01, 0);
	if (ret)
		return ret;
	ret = gc573_it6664_tx_update_bits(dev, GC573_IT6664_TX1_ADDRESS,
					 0x18, BIT(7), BIT(7));
	if (ret)
		return ret;
	ret = gc573_it6664_tx_update_bits(dev, GC573_IT6664_TX1_ADDRESS,
					 0xc1, 0xf0, 0);
	if (ret)
		return ret;
	ret = gc573_it6664_tx_update_bits(dev, GC573_IT6664_TX1_ADDRESS,
					 0xc1, BIT(2), 0);
	if (ret)
		return ret;
	ret = gc573_it6664_tx_update_bits(dev, GC573_IT6664_TX1_ADDRESS,
					 0xad, 0x81, 0);
	if (ret)
		return ret;
	ret = gc573_it6664_tx_update_bits(dev, GC573_IT6664_TX1_ADDRESS,
					 0xa0, 0x30, 0);
	if (ret)
		return ret;
	ret = gc573_it6664_tx_update_bits(dev, GC573_IT6664_SWITCH_ADDRESS,
					 0x0d, 0x0c, 0);
	if (ret)
		return ret;
	if (tx_clock > 340000) {
		ret = gc573_tx1_setup_scdc(dev);
		if (gc573_tx1_stage_error(dev, "initial SCDC setup", ret))
			return ret;
	} else {
		ret = gc573_tx1_clear_scdc(dev);
		if (ret) return ret;
	}
	ret = gc573_it6664_tx_update_bits(dev, GC573_IT6664_TX1_ADDRESS,
					 0x18, 0x0c, 0x0c);
	if (ret)
		return ret;
	ret = gc573_it6664_tx_write(dev, GC573_IT6664_TX1_ADDRESS, 0x85, 0x19);
	if (ret)
		return ret;
	ret = gc573_it6664_tx_update_bits(dev, GC573_IT6664_TX1_ADDRESS,
					 0x1a, 0x0b, 0x0b);
	if (ret)
		return ret;
	ret = gc573_it6664_tx_update_bits(dev, GC573_IT6664_TX1_ADDRESS,
					 0xc1, BIT(3), 0);
	if (ret)
		return ret;
	ret = gc573_it6664_tx_update_bits(dev, GC573_IT6664_TX1_ADDRESS,
					 0xc1, BIT(3), BIT(3));
	if (ret)
		return ret;
	ret = gc573_it6664_tx_update_bits(dev, GC573_IT6664_TX1_ADDRESS,
					 0xc2, BIT(7), BIT(7));
	if (ret)
		return ret;
	ret = gc573_it6664_tx_update_bits(dev, GC573_IT6664_TX1_ADDRESS,
					 0xc3, 0x30, 0x30);
	if (ret)
		return ret;
	ret = gc573_it6664_tx_update_bits(dev, GC573_IT6664_TX1_ADDRESS,
					 0x88, 0x03, 0);
	if (ret)
		return ret;
	msleep(100);
	if (tx_clock > 340000) {
		ret = gc573_tx1_setup_scdc(dev);
		if (gc573_tx1_stage_error(dev, "post-enable SCDC verification", ret))
			return ret;
	} else {
		ret = gc573_tx1_clear_scdc(dev);
		if (ret) return ret;
	}
	/*
	 * Match it6664_enable_tx_output() in gc555-it6664-tx.c: if TX status
	 * bit 3 is still low after output/SCDC setup, reset the video clock,
	 * set C1[7:4] to 0x8, wait 50 ms, and sample status again.
	 */
	ret = gc573_it6664_tx_read(dev, GC573_IT6664_TX1_ADDRESS,
				   GC573_TX_STATUS, &status);
	if (ret)
		return ret;
	if (status == 0xff)
		return -ENODEV;
	if (!(status & BIT(3))) {
		dev_info(&dev->pdev->dev,
			 "TX1 video-clock fallback before reset status=%02x\n",
			 status);
		ret = gc573_it6664_reset_tx_video_clock(dev,
							GC573_IT6664_TX1_ADDRESS);
		if (ret)
			return ret;
		ret = gc573_it6664_tx_update_bits(dev, GC573_IT6664_TX1_ADDRESS,
						 0xc1, 0xf0, 0x80);
		if (ret)
			return ret;
		msleep(50);
		ret = gc573_it6664_tx_read(dev, GC573_IT6664_TX1_ADDRESS,
					   GC573_TX_STATUS, &status);
		if (ret)
			return ret;
		if (status == 0xff)
			return -ENODEV;
		dev_info(&dev->pdev->dev,
			 "TX1 video-clock fallback after reset status=%02x\n",
			 status);
	}
	{
		unsigned long deadline = jiffies + msecs_to_jiffies(5000);

		do {
			ret = gc573_it6664_tx_read(dev, GC573_IT6664_TX1_ADDRESS,
						   GC573_TX_STATUS, &status);
			if (ret)
				return ret;
			if (status == 0xff)
				return -ENODEV;
			if ((status & 0x0f) == 0x0f)
				break;
			msleep(20);
		} while (time_before(jiffies, deadline));
		if ((status & 0x0f) != 0x0f) {
			dev_err(&dev->pdev->dev, "TX1 link timeout status=%02x\n", status);
			return -ENOLINK;
		}
	}
	/* Reference it6664_complete_tx_output(): release the prepared mute. */
	ret = gc573_it6664_tx_update_bits(dev, GC573_IT6664_TX1_ADDRESS,
					 0x91, BIT(4), 0);
	if (ret)
		return ret;
	ret = gc573_it6664_tx_update_bits(dev, GC573_IT6664_TX1_ADDRESS,
					 0xc1, BIT(0), 0);
	if (ret)
		return ret;
	ret = gc573_tx1_wait_receiver_stable(dev);
	if (gc573_tx1_stage_error(dev, "IT6805 receiver settling", ret))
		return ret;
	dev_info(&dev->pdev->dev,
		 "IT6664 TX1 native RGB8 VIC%u %ux%u enabled; RX/TX clocks=%u/%u kHz\n",
		 dev->input_vic, dev->input_width, dev->input_height,
		 rx_clock, tx_clock);
	return 0;
}
