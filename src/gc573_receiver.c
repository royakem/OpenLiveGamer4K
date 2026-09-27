// SPDX-License-Identifier: GPL-2.0-only
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/videodev2.h>

#include "gc573_pure.h"
#include "gc573_it6805_sequence.h"
#include "gc573_acquire.h"
#include "gc573_output.h"
#include "gc573_edid.h"
#include "gc573_audio_status.h"
#include "gc573_audio_clock_math.h"

/* The board API uses the even 8-bit I²C address byte, not a Linux 7-bit ID. */
#define GC573_IT6805_ADDRESS              0x90
#define GC573_IT6805_ID_LENGTH            5
#define GC573_I2C_BUS0                    0

#define GC573_IT6805_REG_BANK             0x0f
#define GC573_IT6805_BANK_MASK            GENMASK(2, 0)
#define GC573_IT6805_BANK_0               0x00
#define GC573_IT6805_BANK_1               0x01
#define GC573_IT6805_BANK_2               0x02
#define GC573_IT6805_BANK_CAOF_PORT0      0x03
#define GC573_IT6805_BANK_CAOF_PORT1_CTL  0x04
#define GC573_IT6805_BANK_CAOF_PORT1      0x07

#define GC573_IT6805_CAOF_DONE_MASK       GENMASK(5, 4)
#define GC573_IT6805_CAOF_PULSE_POLLS     5
#define GC573_IT6805_CAOF_MAX_RESTARTS    6
#define GC573_IT6805_CAOF_DELAY_MS        10

#define GC573_IT6805_OCLK_READY           0x19
#define GC573_IT6805_OCLK_MAX_POLLS       50
#define GC573_IT6805_OCLK_MIN_KHZ         28500
#define GC573_IT6805_OCLK_MAX_KHZ         47500
#define GC573_IT6805_OCLK_DEFAULT_KHZ     38000
#define GC573_IT6805_LOCK_POLLS           500
#define GC573_IT6805_LOCK_DELAY_US        20000
#define GC573_FPGA_REG_GPIO               0x0040
#define GC573_FPGA_GPIO_INPUT_HPD         2

static bool preserve_receiver;
module_param(preserve_receiver, bool, 0444);
MODULE_PARM_DESC(preserve_receiver,
		 "Preserve receiver setup for DMA-only warm-handoff diagnosis");

struct gc573_it6805_caof_result {
	u8 completion[2];
	u8 interrupt[2];
	u16 status[2];
	unsigned int restarts;
};

struct gc573_it6805_oclk_result {
	u32 raw_count;
	u32 clock_khz;
	u32 reference_khz;
	u16 probe[2];
	u16 count[2];
	u8 status_initial;
	u8 status_final;
	u8 selector_offset;
	u8 scale;
	u8 reg91;
	u8 reg92;
	u8 regfd;
	u8 reg44;
	u8 reg45;
	u8 reg46;
	u8 reg47;
	unsigned int polls;
	bool fallback;
};

static int gc573_i2c_read_working(struct gc573_device *dev, u8 bus,
				   u8 address, u8 reg, u8 *value)
{
	if (bus != GC573_I2C_BUS0)
		return -ENODEV;
	return gc573_i2c_read_reg(dev, address, reg, value);
}

static int gc573_i2c_write_working(struct gc573_device *dev, u8 bus,
				    u8 address, u8 reg, u8 value)
{
	if (bus != GC573_I2C_BUS0)
		return -ENODEV;
	return gc573_i2c_write_reg(dev, address, reg, value);
}

const struct gc573_i2c_ops gc573_i2c_board_ops = {
	.read = gc573_i2c_read_working,
	.write = gc573_i2c_write_working,
};

static int gc573_it6805_read(struct gc573_device *dev, u8 reg, u8 *value)
{
	return gc573_i2c_read_reg(dev, GC573_IT6805_ADDRESS, reg, value);
}

static int gc573_it6805_write(struct gc573_device *dev, u8 reg, u8 value)
{
	return gc573_i2c_write_reg(dev, GC573_IT6805_ADDRESS, reg, value);
}

static int gc573_it6805_update_bits(struct gc573_device *dev, u8 reg,
					    u8 mask, u8 value)
{
	u8 old_value;
	int ret;

	ret = gc573_it6805_read(dev, reg, &old_value);
	if (ret)
		return ret;

	return gc573_it6805_write(dev, reg,
					(old_value & ~mask) | (value & mask));
}

static int gc573_it6805_set_bank(struct gc573_device *dev, u8 bank)
{
	if (bank > GC573_IT6805_BANK_MASK)
		return -EINVAL;
	return gc573_it6805_update_bits(dev, GC573_IT6805_REG_BANK,
						GC573_IT6805_BANK_MASK, bank);
}

static int gc573_it6805_apply_updates(struct gc573_device *dev,
					      const struct gc573_it6805_update *updates,
					      size_t count)
{
	size_t i;
	int ret;

	for (i = 0; i < count; i++) {
		ret = gc573_it6805_update_bits(dev, updates[i].reg,
						updates[i].mask, updates[i].value);
		if (ret)
			return ret;
	}
	return 0;
}

static int gc573_it6805_read_identity(struct gc573_device *dev, u8 *id)
{
	if (!id)
		return -EINVAL;

	return gc573_i2c_read_block(dev, GC573_IT6805_ADDRESS, 0x00, id,
					GC573_IT6805_ID_LENGTH);
}

static int gc573_it6805_identify(struct gc573_device *dev)
{
	u8 id[GC573_IT6805_ID_LENGTH];
	int ret;

	ret = gc573_it6805_read_identity(dev, id);
	if (ret)
		return ret;

	dev_info(&dev->pdev->dev,
		 "receiver identity bytes: %02x %02x %02x %02x revision=%02x\n",
		 id[0], id[1], id[2], id[3], id[4]);
	/* Only the measured IT6805 part and revision are supported. */
	if (id[0] != 0x54 || id[1] != 0x49 ||
	    id[2] != 0x05 || id[3] != 0x68 || id[4] != 0xb1) {
		dev_err(&dev->pdev->dev,
			"unsupported receiver identity/revision %02x %02x %02x %02x %02x\n",
			id[0], id[1], id[2], id[3], id[4]);
		return -ENODEV;
	}
	return 0;
}

static const struct gc573_it6805_update gc573_caof_enable[] = {
	{ 0x3a, 0x80, 0x00 },
	{ 0xa0, 0x80, 0x80 },
	{ 0xa1, 0x80, 0x80 },
	{ 0xa2, 0x80, 0x80 },
	{ 0xa4, 0x08, 0x08 },
	{ 0x3b, 0xc0, 0x00 },
	{ 0xa7, 0x10, 0x10 },
	{ 0x48, 0x80, 0x80 },
};

static const struct gc573_it6805_update gc573_caof_disable[] = {
	{ 0x3a, 0x80, 0x00 },
	{ 0xa0, 0x80, 0x00 },
	{ 0xa1, 0x80, 0x00 },
	{ 0xa2, 0x80, 0x00 },
};

static int gc573_it6805_caof_apply(struct gc573_device *dev, u8 bank,
					   const struct gc573_it6805_update *updates,
					   size_t count)
{
	int ret;

	ret = gc573_it6805_set_bank(dev, bank);
	if (ret)
		return ret;
	return gc573_it6805_apply_updates(dev, updates, count);
}

static int gc573_it6805_write_zero_range(struct gc573_device *dev,
						 u8 first_reg, u8 count)
{
	u8 i;
	int ret;

	for (i = 0; i < count; i++) {
		ret = gc573_it6805_write(dev, first_reg + i, 0);
		if (ret)
			return ret;
	}
	return 0;
}

static int gc573_it6805_caof_prepare(struct gc573_device *dev)
{
	int ret;

	ret = gc573_it6805_caof_apply(dev, GC573_IT6805_BANK_CAOF_PORT0,
					      gc573_caof_enable,
					      ARRAY_SIZE(gc573_caof_enable));
	if (ret)
		return ret;
	ret = gc573_it6805_set_bank(dev, GC573_IT6805_BANK_0);
	if (ret)
		return ret;
	ret = gc573_it6805_update_bits(dev, 0x29, 0x01, 0x01);
	if (ret)
		return ret;
	ret = gc573_it6805_update_bits(dev, 0x2a, 0x41, 0x41);
	if (ret)
		return ret;
	msleep(GC573_IT6805_CAOF_DELAY_MS);
	ret = gc573_it6805_update_bits(dev, 0x2a, 0x40, 0x00);
	if (ret)
		return ret;
	ret = gc573_it6805_update_bits(dev, 0x24, 0x04, 0x04);
	if (ret)
		return ret;
	ret = gc573_it6805_write_zero_range(dev, 0x25, 4);
	if (ret)
		return ret;
	ret = gc573_it6805_update_bits(dev, 0x3c, 0x10, 0x00);
	if (ret)
		return ret;

	ret = gc573_it6805_caof_apply(dev, GC573_IT6805_BANK_CAOF_PORT1,
					      gc573_caof_enable,
					      ARRAY_SIZE(gc573_caof_enable));
	if (ret)
		return ret;
	ret = gc573_it6805_set_bank(dev, GC573_IT6805_BANK_0);
	if (ret)
		return ret;
	ret = gc573_it6805_update_bits(dev, 0x32, 0x41, 0x41);
	if (ret)
		return ret;
	msleep(GC573_IT6805_CAOF_DELAY_MS);
	ret = gc573_it6805_update_bits(dev, 0x32, 0x40, 0x00);
	if (ret)
		return ret;
	ret = gc573_it6805_update_bits(dev, 0x2c, 0x04, 0x04);
	if (ret)
		return ret;
	ret = gc573_it6805_write_zero_range(dev, 0x2d, 4);
	if (ret)
		return ret;
	ret = gc573_it6805_set_bank(dev, GC573_IT6805_BANK_CAOF_PORT1_CTL);
	if (ret)
		return ret;
	ret = gc573_it6805_update_bits(dev, 0x3c, 0x10, 0x00);
	if (ret)
		return ret;

	ret = gc573_it6805_set_bank(dev, GC573_IT6805_BANK_CAOF_PORT0);
	if (ret)
		return ret;
	ret = gc573_it6805_update_bits(dev, 0x3a, 0x80, 0x80);
	if (ret)
		return ret;
	ret = gc573_it6805_set_bank(dev, GC573_IT6805_BANK_CAOF_PORT1);
	if (ret)
		return ret;
	ret = gc573_it6805_update_bits(dev, 0x3a, 0x80, 0x80);
	if (ret)
		return ret;

	return gc573_it6805_set_bank(dev, GC573_IT6805_BANK_0);
}

static int gc573_it6805_caof_read_done(struct gc573_device *dev,
					       struct gc573_it6805_caof_result *result)
{
	int ret;

	ret = gc573_it6805_read(dev, 0x08, &result->completion[0]);
	if (ret)
		return ret;
	ret = gc573_it6805_read(dev, 0x0d, &result->completion[1]);
	if (ret)
		return ret;
	result->completion[0] &= GC573_IT6805_CAOF_DONE_MASK;
	result->completion[1] &= GC573_IT6805_CAOF_DONE_MASK;
	return 0;
}

static int gc573_it6805_caof_pulse(struct gc573_device *dev, u8 reg)
{
	int ret;

	ret = gc573_it6805_update_bits(dev, reg, 0x40, 0x40);
	if (ret)
		return ret;
	msleep(GC573_IT6805_CAOF_DELAY_MS);
	return gc573_it6805_update_bits(dev, reg, 0x40, 0x00);
}

static int gc573_it6805_caof_force_retry(struct gc573_device *dev,
						 unsigned int port)
{
	u8 bank = port ? GC573_IT6805_BANK_CAOF_PORT1 :
					GC573_IT6805_BANK_CAOF_PORT0;
	u8 pulse_reg = port ? 0x32 : 0x2a;
	int ret;

	ret = gc573_it6805_set_bank(dev, bank);
	if (ret)
		return ret;
	ret = gc573_it6805_update_bits(dev, 0x3a, 0x80, 0x00);
	if (ret)
		return ret;
	ret = gc573_it6805_set_bank(dev, GC573_IT6805_BANK_0);
	if (ret)
		return ret;
	return gc573_it6805_caof_pulse(dev, pulse_reg);
}

static int gc573_it6805_caof_wait(struct gc573_device *dev,
					  struct gc573_it6805_caof_result *result)
{
	unsigned int polls = 0;
	int ret;

	ret = gc573_it6805_caof_read_done(dev, result);
	if (ret)
		return ret;

	while ((!result->completion[0] || !result->completion[1]) &&
	       result->restarts < GC573_IT6805_CAOF_MAX_RESTARTS) {
		ret = gc573_it6805_caof_read_done(dev, result);
		if (ret)
			return ret;

		if (polls >= GC573_IT6805_CAOF_PULSE_POLLS) {
			if (!result->completion[0]) {
				ret = gc573_it6805_caof_pulse(dev, 0x2a);
				if (ret)
					return ret;
			}
			if (!result->completion[1]) {
				ret = gc573_it6805_caof_pulse(dev, 0x32);
				if (ret)
					return ret;
			}
			polls = 0;
			result->restarts++;
		}
		msleep(GC573_IT6805_CAOF_DELAY_MS);
		polls++;
	}

	if (result->completion[0] && result->completion[1])
		return 0;
	if (!result->completion[0]) {
		ret = gc573_it6805_caof_force_retry(dev, 0);
		if (ret)
			return ret;
	}
	if (!result->completion[1]) {
		ret = gc573_it6805_caof_force_retry(dev, 1);
		if (ret)
			return ret;
	}

	ret = gc573_it6805_caof_read_done(dev, result);
	if (ret)
		return ret;
	return result->completion[0] && result->completion[1] ? 0 : -ETIMEDOUT;
}

static int gc573_it6805_caof_cleanup(struct gc573_device *dev)
{
	int ret;

	ret = gc573_it6805_set_bank(dev, GC573_IT6805_BANK_0);
	if (ret)
		return ret;
	ret = gc573_it6805_update_bits(dev, 0x08, 0x30, 0x30);
	if (ret)
		return ret;
	ret = gc573_it6805_update_bits(dev, 0x0d, 0x30, 0x30);
	if (ret)
		return ret;
	ret = gc573_it6805_update_bits(dev, 0x29, 0x01, 0x00);
	if (ret)
		return ret;
	ret = gc573_it6805_update_bits(dev, 0x24, 0x04, 0x00);
	if (ret)
		return ret;
	ret = gc573_it6805_update_bits(dev, 0x3c, 0x10, 0x10);
	if (ret)
		return ret;
	ret = gc573_it6805_update_bits(dev, 0x2c, 0x04, 0x00);
	if (ret)
		return ret;
	ret = gc573_it6805_set_bank(dev, GC573_IT6805_BANK_CAOF_PORT1_CTL);
	if (ret)
		return ret;
	ret = gc573_it6805_update_bits(dev, 0x3c, 0x10, 0x10);
	if (ret)
		return ret;
	ret = gc573_it6805_caof_apply(dev, GC573_IT6805_BANK_CAOF_PORT0,
					      gc573_caof_disable,
					      ARRAY_SIZE(gc573_caof_disable));
	if (ret)
		return ret;
	ret = gc573_it6805_caof_apply(dev, GC573_IT6805_BANK_CAOF_PORT1,
					      gc573_caof_disable,
					      ARRAY_SIZE(gc573_caof_disable));
	if (ret)
		return ret;
	return gc573_it6805_set_bank(dev, GC573_IT6805_BANK_0);
}

static int gc573_it6805_run_caof(struct gc573_device *dev)
{
	struct gc573_it6805_caof_result result = {};
	int cleanup_ret;
	int ret;

	ret = gc573_it6805_caof_prepare(dev);
	if (!ret)
		ret = gc573_it6805_caof_wait(dev, &result);
	cleanup_ret = gc573_it6805_caof_cleanup(dev);
	if (!ret)
		ret = cleanup_ret;
	if (ret)
		gc573_it6805_set_bank(dev, GC573_IT6805_BANK_0);

	dev_info(&dev->pdev->dev,
		 "IT6805 CAOF completion=%02x/%02x status=%04x/%04x restarts=%u ret=%d\n",
		 result.completion[0], result.completion[1], result.status[0],
		 result.status[1], result.restarts, ret);
	return ret;
}

static int gc573_it6805_oclk_read_selector(struct gc573_device *dev,
						   u8 offset, u8 selector,
						   u16 *value)
{
	u8 low;
	u8 high;
	int ret;

	ret = gc573_it6805_write(dev, 0x50, offset);
	if (ret)
		return ret;
	ret = gc573_it6805_write(dev, 0x51, selector);
	if (ret)
		return ret;
	ret = gc573_it6805_write(dev, 0x54, 0x04);
	if (ret)
		return ret;
	ret = gc573_it6805_read(dev, 0x61, &low);
	if (ret)
		return ret;
	ret = gc573_it6805_read(dev, 0x62, &high);
	if (ret)
		return ret;
	*value = ((u16)high << 8) | low;
	return 0;
}

static int gc573_it6805_oclk_cleanup(struct gc573_device *dev)
{
	int ret;
	int bank0_ret;

	ret = gc573_it6805_set_bank(dev, GC573_IT6805_BANK_1);
	if (!ret)
		ret = gc573_it6805_write(dev, 0x5f, 0x00);
	bank0_ret = gc573_it6805_set_bank(dev, GC573_IT6805_BANK_0);
	if (!bank0_ret)
		bank0_ret = gc573_it6805_write(dev, 0xf8, 0x00);
	return ret ? ret : bank0_ret;
}

static int gc573_it6805_oclk_load(struct gc573_device *dev,
					  struct gc573_it6805_oclk_result *result)
{
	u32 measured_khz;
	unsigned int i;
	int cleanup_ret;
	int ret;

	ret = gc573_it6805_set_bank(dev, GC573_IT6805_BANK_0);
	if (ret)
		goto cleanup;
	ret = gc573_it6805_write(dev, 0xf8, 0xc3);
	if (ret)
		goto cleanup;
	ret = gc573_it6805_write(dev, 0xf8, 0xa5);
	if (ret)
		goto cleanup;
	ret = gc573_it6805_write(dev, 0x34, 0x00);
	if (ret)
		goto cleanup;
	ret = gc573_it6805_set_bank(dev, GC573_IT6805_BANK_1);
	if (ret)
		goto cleanup;
	ret = gc573_it6805_write(dev, 0x5f, 0x04);
	if (ret)
		goto cleanup;
	ret = gc573_it6805_write(dev, 0x5f, 0x05);
	if (ret)
		goto cleanup;
	ret = gc573_it6805_write(dev, 0x58, 0x12);
	if (ret)
		goto cleanup;
	ret = gc573_it6805_write(dev, 0x58, 0x02);
	if (ret)
		goto cleanup;
	ret = gc573_it6805_read(dev, 0x60, &result->status_initial);
	if (ret)
		goto cleanup;
	result->status_final = result->status_initial;

	if (result->status_initial != GC573_IT6805_OCLK_READY) {
		ret = gc573_it6805_write(dev, 0xf8, 0xc3);
		if (ret)
			goto cleanup;
		ret = gc573_it6805_write(dev, 0xf8, 0xa5);
		if (ret)
			goto cleanup;
		ret = gc573_it6805_write(dev, 0x5f, 0x04);
		if (ret)
			goto cleanup;
		ret = gc573_it6805_write(dev, 0x58, 0x12);
		if (ret)
			goto cleanup;
		ret = gc573_it6805_write(dev, 0x58, 0x02);
		if (ret)
			goto cleanup;
		for (i = 0; i < GC573_IT6805_OCLK_MAX_POLLS; i++) {
			ret = gc573_it6805_read(dev, 0x60,
							&result->status_final);
			if (ret)
				goto cleanup;
			result->polls = i + 1;
			if (result->status_final == GC573_IT6805_OCLK_READY)
				break;
			usleep_range(1000, 2000);
		}
		if (result->status_final != GC573_IT6805_OCLK_READY) {
			ret = -ETIMEDOUT;
			goto cleanup;
		}
		usleep_range(10000, 11000);
		ret = gc573_it6805_set_bank(dev, GC573_IT6805_BANK_0);
		if (ret)
			goto cleanup;
		ret = gc573_it6805_update_bits(dev, 0xcf, 0x01, 0x01);
		if (ret)
			goto cleanup;
		ret = gc573_it6805_set_bank(dev, GC573_IT6805_BANK_1);
		if (ret)
			goto cleanup;
	}

	ret = gc573_it6805_write(dev, 0x57, 0x01);
	if (ret)
		goto cleanup;
	ret = gc573_it6805_oclk_read_selector(dev, 0x00, 0x00,
						      &result->probe[0]);
	if (ret)
		goto cleanup;
	ret = gc573_it6805_oclk_read_selector(dev, 0x00, 0x01,
						      &result->probe[1]);
	if (ret)
		goto cleanup;
	if (result->probe[0] == 0xffff && !result->probe[1])
		result->selector_offset = 0x04;

	ret = gc573_it6805_oclk_read_selector(dev, result->selector_offset,
						      0xb0, &result->count[0]);
	if (ret)
		goto cleanup;
	ret = gc573_it6805_oclk_read_selector(dev, result->selector_offset,
						      0xb1, &result->count[1]);
	if (ret)
		goto cleanup;

	result->scale = result->count[1] >> 8;
	result->raw_count = ((u32)(result->count[1] & 0xff) << 16) |
				result->count[0];
	measured_khz = result->raw_count;
	if ((result->scale & 0xc0) == 0xc0)
		measured_khz /= 100;
	if (measured_khz < GC573_IT6805_OCLK_MIN_KHZ ||
	    measured_khz > GC573_IT6805_OCLK_MAX_KHZ) {
		dev_warn(&dev->pdev->dev,
			 "invalid OCLK measurement %u kHz (raw=0x%x); using %u kHz fallback\n",
			 measured_khz, result->raw_count,
			 GC573_IT6805_OCLK_DEFAULT_KHZ);
		measured_khz = GC573_IT6805_OCLK_DEFAULT_KHZ;
		result->fallback = true;
	}
	result->clock_khz = measured_khz;

cleanup:
	cleanup_ret = gc573_it6805_oclk_cleanup(dev);
	return ret ? ret : cleanup_ret;
}

static int gc573_it6805_oclk_program(struct gc573_device *dev,
					     struct gc573_it6805_oclk_result *result)
{
	u32 timer_khz;
	int ret;

	/* The shipped GC573 routine calibrates its observed revision dynamically. */
	timer_khz = result->clock_khz / 20 + (result->clock_khz >> 1);
	result->reference_khz = result->clock_khz >> 1;
	result->reg91 = (timer_khz / 1000) & 0x3f;
	result->reg92 = ((timer_khz % 1000) * 0x100) / 1000;
	result->regfd = (result->clock_khz >> 3) / 25;
	result->reg45 = (result->clock_khz >> 3) / 39;
	result->reg44 = result->clock_khz / 1560;
	result->reg46 = result->clock_khz / 2320;
	result->reg47 = result->clock_khz / 5312;

	ret = gc573_it6805_set_bank(dev, GC573_IT6805_BANK_CAOF_PORT0);
	if (ret)
		return ret;
	ret = gc573_it6805_update_bits(dev, 0xaa, 0x1f, 0x0c);
	if (ret)
		return ret;
	ret = gc573_it6805_set_bank(dev, GC573_IT6805_BANK_0);
	if (ret)
		return ret;
	ret = gc573_it6805_update_bits(dev, 0x91, 0x3f, result->reg91);
	if (ret)
		return ret;
	ret = gc573_it6805_write(dev, 0x92, result->reg92);
	if (ret)
		return ret;
	ret = gc573_it6805_set_bank(dev, GC573_IT6805_BANK_1);
	if (ret)
		return ret;
	ret = gc573_it6805_write(dev, 0xfd, result->regfd);
	if (ret)
		return ret;
	ret = gc573_it6805_update_bits(dev, 0xfe, 0x20, 0x00);
	if (ret)
		return ret;
	ret = gc573_it6805_update_bits(dev, 0xfe, 0x0f, 0x0c);
	if (ret)
		return ret;
	ret = gc573_it6805_update_bits(dev, 0xfe, 0x10, 0x10);
	if (ret)
		return ret;
	ret = gc573_it6805_update_bits(dev, 0xfe, 0x80, 0x80);
	if (ret)
		return ret;
	ret = gc573_it6805_set_bank(dev, GC573_IT6805_BANK_0);
	if (ret)
		return ret;
	ret = gc573_it6805_write(dev, 0x45, result->reg45);
	if (ret)
		return ret;
	ret = gc573_it6805_write(dev, 0x44, result->reg44);
	if (ret)
		return ret;
	ret = gc573_it6805_write(dev, 0x46, result->reg46);
	if (ret)
		return ret;
	ret = gc573_it6805_write(dev, 0x47, result->reg47);
	if (ret)
		return ret;
	return gc573_it6805_set_bank(dev, GC573_IT6805_BANK_0);
}

static int gc573_it6805_calibrate_oclk(struct gc573_device *dev,
					      bool *used_fallback)
{
	struct gc573_it6805_oclk_result result = {};
	int ret;

	ret = gc573_it6805_oclk_load(dev, &result);
	if (used_fallback)
		*used_fallback = result.fallback;
	if (!ret && result.fallback)
		dev_warn(&dev->pdev->dev,
			 "IT6805 OCLK calibration using documented fallback %u kHz\n",
			 result.clock_khz);
	if (!ret)
		ret = gc573_it6805_oclk_program(dev, &result);
	if (ret)
		gc573_it6805_set_bank(dev, GC573_IT6805_BANK_0);
	else
		dev->oclk_reference_khz = result.reference_khz;

	dev_info(&dev->pdev->dev,
		 "IT6805 OCLK=%u kHz raw=0x%x status=%02x/%02x probes=%04x/%04x count=%04x/%04x fallback=%u ret=%d\n",
		 result.clock_khz, result.raw_count, result.status_initial,
		 result.status_final, result.probe[0], result.probe[1],
		 result.count[0], result.count[1], result.fallback, ret);
	return ret;
}

static int gc573_it6805_set_ttl_path(struct gc573_device *dev)
{
	int ret;

	ret = gc573_it6805_set_bank(dev, GC573_IT6805_BANK_1);
	if (ret)
		return ret;
	ret = gc573_it6805_update_bits(dev, 0xc0, 0x06, 0x02);
	if (ret)
		return ret;
	ret = gc573_it6805_update_bits(dev, 0xc1, 0x02, 0x02);
	if (ret)
		return ret;
	ret = gc573_it6805_update_bits(dev, 0xc1, 0x20, 0x00);
	if (ret)
		return ret;
	return gc573_it6805_set_bank(dev, GC573_IT6805_BANK_0);
}

static int gc573_it6805_select_port0(struct gc573_device *dev)
{
	u8 status13;
	u8 status16;
	int ret;

	ret = gc573_it6805_set_bank(dev, GC573_IT6805_BANK_0);
	if (ret)
		return ret;
	ret = gc573_it6805_read(dev, 0x13, &status13);
	if (ret)
		return ret;
	ret = gc573_it6805_read(dev, 0x16, &status16);
	if (ret)
		return ret;
	if (!(status13 & BIT(0)) && (status16 & BIT(0)))
		return -EOPNOTSUPP;

	ret = gc573_it6805_update_bits(dev, 0x35, BIT(0), 0);
	if (ret)
		return ret;
	ret = gc573_it6805_set_bank(dev, GC573_IT6805_BANK_CAOF_PORT0);
	if (ret)
		return ret;
	ret = gc573_it6805_update_bits(dev, 0xe5, 0x1c, 0);
	if (ret)
		return ret;
	ret = gc573_it6805_set_bank(dev, GC573_IT6805_BANK_CAOF_PORT1);
	if (ret)
		return ret;
	ret = gc573_it6805_update_bits(dev, 0xe5, 0x1c, 0);
	if (ret)
		return ret;
	ret = gc573_it6805_set_bank(dev, GC573_IT6805_BANK_0);
	if (ret)
		return ret;
	ret = gc573_it6805_write(dev, 0x25, 0x00);
	if (ret)
		return ret;
	ret = gc573_it6805_write(dev, 0x26, 0x00);
	if (ret)
		return ret;
	ret = gc573_it6805_write(dev, 0x27, 0x00);
	if (ret)
		return ret;
	ret = gc573_it6805_write(dev, 0x2a, 0x01);
	if (ret)
		return ret;
	ret = gc573_it6805_write(dev, 0x2d, 0xff);
	if (ret)
		return ret;
	ret = gc573_it6805_write(dev, 0x2e, 0xff);
	if (ret)
		return ret;
	ret = gc573_it6805_write(dev, 0x2f, 0xff);
	if (ret)
		return ret;
	ret = gc573_it6805_write(dev, 0x32, 0x3e);
	if (ret)
		return ret;
	ret = gc573_it6805_set_bank(dev, GC573_IT6805_BANK_CAOF_PORT0);
	if (ret)
		return ret;
	ret = gc573_it6805_update_bits(dev, 0xa8, BIT(3), BIT(3));
	if (ret)
		return ret;
	ret = gc573_it6805_set_bank(dev, GC573_IT6805_BANK_CAOF_PORT1);
	if (ret)
		return ret;
	ret = gc573_it6805_update_bits(dev, 0xa8, BIT(3), 0);
	if (ret)
		return ret;
	ret = gc573_it6805_set_bank(dev, GC573_IT6805_BANK_0);
	if (ret)
		return ret;
	ret = gc573_it6805_update_bits(dev, 0xc5, BIT(4), BIT(4));
	if (ret)
		return ret;
	usleep_range(1000, 2000);
	return gc573_it6805_update_bits(dev, 0xc5, BIT(4), 0);
}

static int gc573_it6805_reset_eq_port0(struct gc573_device *dev)
{
	int ret;

	ret = gc573_it6805_set_bank(dev, GC573_IT6805_BANK_CAOF_PORT0);
	if (ret)
		return ret;
	ret = gc573_it6805_write(dev, 0x2c, 0x00);
	if (ret)
		return ret;
	ret = gc573_it6805_write(dev, 0x2d, 0x07);
	if (ret)
		return ret;
	ret = gc573_it6805_set_bank(dev, GC573_IT6805_BANK_0);
	if (ret)
		return ret;
	ret = gc573_it6805_write(dev, 0x07, 0xff);
	if (ret)
		return ret;
	ret = gc573_it6805_write(dev, 0x23, 0xb0);
	if (ret)
		return ret;
	usleep_range(1000, 2000);
	ret = gc573_it6805_write(dev, 0x23, 0xa0);
	if (ret)
		return ret;
	ret = gc573_it6805_update_bits(dev, 0x23, BIT(1), BIT(1));
	if (ret)
		return ret;
	ret = gc573_it6805_set_bank(dev, GC573_IT6805_BANK_CAOF_PORT0);
	if (ret)
		return ret;
	ret = gc573_it6805_write(dev, 0x27, 0x9f);
	if (ret)
		return ret;
	ret = gc573_it6805_write(dev, 0x28, 0x9f);
	if (ret)
		return ret;
	ret = gc573_it6805_write(dev, 0x29, 0x9f);
	if (ret)
		return ret;
	ret = gc573_it6805_write(dev, 0x22, 0x00);
	if (ret)
		return ret;
	ret = gc573_it6805_update_bits(dev, 0x4b, BIT(7), 0);
	if (ret)
		return ret;
	return gc573_it6805_set_bank(dev, GC573_IT6805_BANK_0);
}

static int gc573_it6805_start_eq_port0(struct gc573_device *dev)
{
	u8 status;
	int ret;

	ret = gc573_it6805_set_bank(dev, GC573_IT6805_BANK_0);
	if (ret)
		return ret;
	ret = gc573_it6805_read(dev, 0x14, &status);
	if (ret)
		return ret;
	ret = gc573_it6805_set_bank(dev, GC573_IT6805_BANK_CAOF_PORT0);
	if (ret)
		return ret;
	ret = gc573_it6805_update_bits(dev, 0xa7, BIT(6),
					      status & (BIT(6) | BIT(0)) ?
					      BIT(6) : 0);
	if (ret)
		return ret;
	return gc573_it6805_set_bank(dev, GC573_IT6805_BANK_0);
}

static int gc573_it6805_handle_5v_port0(struct gc573_device *dev)
{
	u8 status;
	int ret;

	ret = gc573_it6805_set_bank(dev, GC573_IT6805_BANK_0);
	if (ret)
		return ret;
	ret = gc573_it6805_read(dev, 0x13, &status);
	if (ret)
		return ret;
	if (status & BIT(0)) {
		ret = gc573_mmio_maskwrite32(&dev->mmio, GC573_FPGA_REG_GPIO,
					    BIT(GC573_FPGA_GPIO_INPUT_HPD),
					    BIT(GC573_FPGA_GPIO_INPUT_HPD));
		if (ret)
			return ret;
		dev_info(&dev->pdev->dev,
			 "IT6805 port-0 5V present; source HPD asserted on GPIO2\n");
	} else {
		dev_info(&dev->pdev->dev,
			 "IT6805 port-0 5V absent; source HPD left unchanged\n");
	}
	return 0;
}

static int gc573_it6805_initialize_input_path(struct gc573_device *dev)
{
	int ret;

	ret = gc573_it6805_set_ttl_path(dev);
	if (ret)
		return ret;
	ret = gc573_it6805_select_port0(dev);
	if (ret)
		goto cleanup;
	ret = gc573_it6805_reset_eq_port0(dev);
	if (ret)
		goto cleanup;
	ret = gc573_it6805_start_eq_port0(dev);
	if (ret)
		goto cleanup;
	ret = gc573_it6805_trigger_eq(dev);
	if (ret)
		goto cleanup;
	ret = gc573_it6805_handle_5v_port0(dev);

cleanup:
	if (ret)
		gc573_it6805_set_bank(dev, GC573_IT6805_BANK_0);
	return ret;
}

static int gc573_it6805_status(struct gc573_device *dev, bool *locked);

static int gc573_it6805_wait_for_lock(struct gc573_device *dev)
{
	unsigned int poll;
	int ret;

	for (poll = 0; poll < GC573_IT6805_LOCK_POLLS; poll++) {
		bool locked;

		if (!preserve_receiver) {
			ret = gc573_it6805_service_acquisition(dev);
			if (ret)
				return ret;
		}
		ret = gc573_it6805_status(dev, &locked);
		if (ret)
			return ret;
		if (locked)
			return 0;
		usleep_range(GC573_IT6805_LOCK_DELAY_US,
			     GC573_IT6805_LOCK_DELAY_US + 1000);
	}
	{
		u8 state[16];

		ret = gc573_i2c_read_block(dev, GC573_IT6805_ADDRESS, 0x10,
					  state, sizeof(state));
		if (!ret)
			dev_warn(&dev->pdev->dev,
				 "lock timeout: bank0 regs10..1f=%*ph\n",
				 (int)sizeof(state), state);
	}
	return -ETIMEDOUT;
}

static int gc573_it6805_set_video_tristate(struct gc573_device *dev,
						 bool tristate)
{
	int ret;

	ret = gc573_it6805_set_bank(dev, GC573_IT6805_BANK_1);
	if (ret)
		return ret;
	ret = gc573_it6805_update_bits(dev, 0xc5, BIT(7),
					      tristate ? BIT(7) : 0);
	if (ret)
		return ret;
	ret = gc573_it6805_update_bits(dev, 0xc6, BIT(7),
					      tristate ? BIT(7) : 0);
	if (ret)
		return ret;
	return gc573_it6805_set_bank(dev, GC573_IT6805_BANK_0);
}

static int gc573_it6805_initialize(struct gc573_device *dev)
{
	bool oclk_fallback = false;
	u8 output_state[12];
	int ret, cleanup_ret;

	ret = gc573_it6805_set_bank(dev, GC573_IT6805_BANK_1);
	if (!ret)
		ret = gc573_i2c_read_block(dev, GC573_IT6805_ADDRESS, 0xc0,
					  output_state, sizeof(output_state));
	if (!ret)
		dev_info(&dev->pdev->dev, "pre-init bank1 c0..cb=%*ph\n",
			 (int)sizeof(output_state), output_state);
	cleanup_ret = gc573_it6805_set_bank(dev, GC573_IT6805_BANK_0);
	if (!ret)
		ret = cleanup_ret;
	if (ret)
		return ret;

	if (preserve_receiver) {
		dev_warn(&dev->pdev->dev,
			 "preserve_receiver=1: inherited receiver state; DMA diagnostic only\n");
		return 0;
	}
	/* Renegotiate the capture input after resetting its HDMI receiver. */
	ret = gc573_mmio_maskwrite32(&dev->mmio, GC573_FPGA_REG_GPIO,
				    BIT(GC573_FPGA_GPIO_INPUT_HPD), 0);
	if (ret)
		return ret;
	msleep(100);

	/* This table is the exact source-level expansion of the shipped object. */
	ret = gc573_it6805_apply_updates(dev, gc573_it6805_initial_sequence,
					 ARRAY_SIZE(gc573_it6805_initial_sequence));
	if (ret)
		return ret;

	ret = gc573_it6805_run_caof(dev);
	if (ret)
		return ret;
	ret = gc573_it6805_set_bank(dev, GC573_IT6805_BANK_0);
	if (ret)
		return ret;
	ret = gc573_it6805_write(dev, 0x28, 0x88);
	if (ret)
		return ret;
	ret = gc573_it6805_set_video_tristate(dev, true);
	if (ret)
		return ret;

	ret = gc573_it6805_calibrate_oclk(dev, &oclk_fallback);
	if (ret)
		return ret;
	ret = gc573_it6805_program_edid(dev);
	if (ret)
		return ret;
	ret = gc573_it6805_initialize_input_path(dev);
	if (ret)
		return ret;

	if (oclk_fallback)
		dev_warn(&dev->pdev->dev,
			 "IT6805 source initialization complete with degraded 38 MHz OCLK fallback; video tristated\n");
	else
		dev_info(&dev->pdev->dev,
			 "IT6805 source initialization complete; video tristated\n");
	return 0;
}

static int gc573_it6805_status(struct gc573_device *dev, bool *locked)
{
	u8 five_volt;
	u8 scdt;
	int ret;

	if (!locked)
		return -EINVAL;
	ret = gc573_it6805_set_bank(dev, GC573_IT6805_BANK_0);
	if (ret)
		return ret;
	ret = gc573_it6805_read(dev, 0x13, &five_volt);
	if (ret)
		return ret;
	ret = gc573_it6805_read(dev, 0x19, &scdt);
	if (ret)
		return ret;
	/* iTE6805_Check_SCDT tests bank-0 register 0x19 bit 7. */
	*locked = five_volt != 0xff && scdt != 0xff &&
		  !!(five_volt & BIT(0)) && !!(scdt & BIT(7));
	dev_dbg(&dev->pdev->dev,
		 "IT6805 signal status 5V=%02x SCDT=%02x locked=%u\n",
		 five_volt, scdt, *locked);
	return 0;
}

int gc573_receiver_audio_snapshot(struct gc573_device *dev,
				  struct gc573_audio_status *status)
{
	struct gc573_audio_status sample = {};
	u8 original_latch = 0;
	u8 n_cts_shared = 0;
	bool have_latch = false;
	int cleanup_ret;
	int ret;

	if (!dev || !status || !dev->vdev)
		return -EINVAL;

	/*
	 * The caller holds dev->vdev->lock and this function takes dev->lock.
	 * Keep the entire banked transaction under those serializations and
	 * restore bank 0 on every exit path. Do not
	 * read common IRQ status (bank 0, 0x10): it is interrupt state, not a
	 * passive audio-presence register.
	 */
	mutex_lock(&dev->lock);
	ret = gc573_it6805_set_bank(dev, GC573_IT6805_BANK_0);
	if (!ret)
		ret = gc573_it6805_read(dev, 0x86, &original_latch);
	if (!ret)
		have_latch = true;
	if (!ret)
		ret = gc573_it6805_update_bits(dev, 0x86, BIT(0), BIT(0));
	if (!ret)
		ret = gc573_it6805_read(dev, 0x19, &sample.receiver_scdt_19);
	if (!ret)
		ret = gc573_it6805_read(dev, 0xb0, &sample.infoframe_b0);
	if (!ret)
		ret = gc573_it6805_read(dev, 0xb1, &sample.infoframe_b1);
	if (!ret)
		ret = gc573_it6805_read(dev, 0xb2, &sample.infoframe_b2);
	if (!ret)
		ret = gc573_it6805_read(dev, 0xb5, &sample.audio_rate_b5);
	if (!ret)
		ret = gc573_it6805_read(dev, 0xb6, &sample.audio_rate_b6);
	/* Control registers used by reference masked updates; no IRQ reads. */
	if (!ret)
		ret = gc573_it6805_read(dev, 0x81, &sample.audio_control_81);
	if (!ret)
		ret = gc573_it6805_read(dev, 0x8a, &sample.audio_control_8a);
	if (!ret)
		ret = gc573_it6805_read(dev, 0x8c, &sample.audio_control_8c);
	if (!ret)
		ret = gc573_it6805_set_bank(dev, GC573_IT6805_BANK_1);
	if (!ret)
		ret = gc573_it6805_read(dev, 0xc7, &sample.audio_output_c7);
	if (!ret)
		ret = gc573_it6805_set_bank(dev, 0x02);
	if (!ret)
		ret = gc573_it6805_read(dev, 0xbe, &sample.n_high);
	if (!ret)
		ret = gc573_it6805_read(dev, 0xbf, &sample.n_mid);
	if (!ret)
		ret = gc573_it6805_read(dev, 0xc0, &n_cts_shared);
	if (!ret) {
		sample.n_low = n_cts_shared;
		sample.cts_low = n_cts_shared;
	}
	if (!ret)
		ret = gc573_it6805_read(dev, 0xc1, &sample.cts_mid);
	if (!ret)
		ret = gc573_it6805_read(dev, 0xc2, &sample.cts_high);

	/* Restore the reference latch bit even after a partial sampling failure. */
	if (have_latch) {
		cleanup_ret = gc573_it6805_set_bank(dev, GC573_IT6805_BANK_0);
		if (!ret)
			ret = cleanup_ret;
		if (!cleanup_ret) {
			cleanup_ret = gc573_it6805_update_bits(dev, 0x86, BIT(0),
							       original_latch & BIT(0));
			if (!ret)
				ret = cleanup_ret;
		}
	}
	cleanup_ret = gc573_it6805_set_bank(dev, GC573_IT6805_BANK_0);
	if (!ret)
		ret = cleanup_ret;
	if (ret) {
		mutex_unlock(&dev->lock);
		return ret;
	}
	sample.n_decoded = ((u32)sample.n_high << 12) |
			   ((u32)sample.n_mid << 4) | (n_cts_shared & 0x0f);
	sample.cts_decoded = ((u32)sample.cts_mid << 12) |
			     ((u32)sample.cts_high << 4) | (n_cts_shared >> 4);
	sample.counters_coherent = true;

	/* B1 bit 7 is the reference driver's audio-info readiness condition. */
	sample.infoframe_valid = !!(sample.infoframe_b1 & BIT(7));
	/* Bank-0 register 0x19 bit 7 is the documented SCDT lock indication. */
	sample.video_scdt = !!(sample.receiver_scdt_19 & BIT(7));
	/* Two distinct decodes, kept deliberately separate:
	 *   - receiver_rate_code: vendor diagnostic readout
	 *     (iTE6805_Show_AUD_Info, ite6805_drv.o 0x38c0/0x38df), which
	 *     combines B5 with itself: ((B5>>2)&0x30)|(B5&0x0f).
	 *   - gc573_it6805_decode_receiver_rate(b5, b6): vendor *functional*
	 *     decode (iTE6805_Enable_Audio_Output) and reference
	 *     it6805_update_audio_clock_locked: (B6>>2&0x30)|(B5&0x0f).
	 * The admission gate uses the diagnostic rate; the functional code may
	 * disagree on a locked Deck source (B5=02/B6=db). The clock path must
	 * independently establish stable measured 48 kHz before output starts.
	 */
	sample.receiver_rate_code =
		((sample.audio_rate_b5 >> 2) & 0x30) |
		(sample.audio_rate_b5 & 0x0f);
	/* Reference channel logic treats B0 bit 6 as stereo, else CA=1 is 2ch. */
	sample.receiver_reports_48k_lpcm_stereo =
		sample.infoframe_valid && (sample.infoframe_b0 >> 4) == 0 &&
		((sample.infoframe_b0 & BIT(6)) ||
		 (sample.infoframe_b1 & 0x3f) == 0x01) &&
		sample.receiver_rate_code == GC573_IT6805_AUDIO_RATE_48_KHZ;
	*status = sample;
	mutex_unlock(&dev->lock);
	return 0;
}

int gc573_receiver_untristate_audio(struct gc573_device *dev)
{
	int cleanup_ret;
	int ret;

	if (!dev || !dev->vdev)
		return -EINVAL;

	/* Mirrors the reference set_audio_tristate(false), with bank cleanup. */
	mutex_lock(&dev->lock);
	ret = gc573_it6805_set_bank(dev, GC573_IT6805_BANK_1);
	if (!ret)
		ret = gc573_it6805_write(dev, 0xc7, 0x00);
	cleanup_ret = gc573_it6805_set_bank(dev, GC573_IT6805_BANK_0);
	if (!ret)
		ret = cleanup_ret;
	mutex_unlock(&dev->lock);
	return ret;
}

/*
 * Experimental audio clock path, ported from the reference receiver driver
 * (livegamer4k/internal/gc555-reference/gc555-it6805-core.c) and cross-checked
 * against the shipped GC573 vendor object AverMediaLib_64.a:ite6805_sys.o,
 * iTE6805_Enable_Audio_Output (0x499e). This is the step the bounded
 * prototype previously omitted: latch N/CTS, measure the audio TMDS clock,
 * derive the computed sample-rate code, decode the receiver's internal rate
 * code, and force the 0x81[6]/0x8a[5:0] override on a confirmed mismatch.
 *
 * Provenance per function:
 *   read_ncts       <- it6805_read_audio_clock_locked (3356-3395)
 *   audio_rate_code <- it6805_audio_rate_code (3447-3477)
 *   measure_tmds    <- it6805_measure_audio_tmds_locked (3503-3539)
 *   reset_audio_logic <- it6805_reset_audio_logic_locked (3413-3430) / vendor
 *                        iTE6805_Reset_Audio_Logic (0x1bac)
 *   update_clock    <- it6805_update_audio_clock_locked (3541-3609) / vendor
 *
 * All of these run with dev->lock already held by the caller and leave the
 * receiver in bank 0. prepare_audio is a one-shot start op, so the reference's
 * runtime 16-poll mismatch counter is replicated inside a single
 * update_audio_clock call: re-measure and re-compare up to 16 times, force
 * only while the mismatch persists (and only for the 48 kHz code, per the
 * stereo48k-only contract). Every value is logged for the bench.
 */

/*
 * Caller holds dev->lock. Latch N/CTS using bank-0 0x86 bit 0. The bank-2
 * read and the 0x86 latch restore are both attempted on every path after the
 * latch was set; the first data error is preserved and a failed restore is
 * reported separately. 0x86 is a bank-0 register, so the latch restore only
 * runs when bank 0 is confirmed current (or was never left); if returning to
 * bank 0 failed, the restore is skipped and reported rather than written to
 * the wrong bank.
 */
static int gc573_it6805_read_ncts(struct gc573_device *dev,
				  u32 *n, u32 *cts)
{
	u8 n_high, n_mid, n_cts_shared, cts_low, cts_mid, cts_high;
	u8 original_latch;
	bool bank0_known;
	int cleanup_ret;
	int ret;

	ret = gc573_it6805_read(dev, 0x86, &original_latch);
	if (ret)
		return ret;
	ret = gc573_it6805_update_bits(dev, 0x86, BIT(0), BIT(0));
	if (ret)
		return ret;
	/* N/CTS live in bank 2 (reference it6805_read_audio_clock_locked and
	 * vendor iTE6805_Enable_Audio_Output both select bank 0x02 here). */
	bank0_known = false;
	ret = gc573_it6805_set_bank(dev, GC573_IT6805_BANK_2);
	if (!ret)
		ret = gc573_it6805_read(dev, 0xbe, &n_high);
	if (!ret)
		ret = gc573_it6805_read(dev, 0xbf, &n_mid);
	/* 0xc0 is read twice, as in the reference: the first read provides the
	 * N low nibble, the second the CTS low nibble. Packing matches the
	 * reference line for line; the latch semantics themselves are
	 * reference-derived and remain pending hardware verification. */
	if (!ret)
		ret = gc573_it6805_read(dev, 0xc0, &n_cts_shared);
	if (!ret)
		ret = gc573_it6805_read(dev, 0xc0, &cts_low);
	if (!ret)
		ret = gc573_it6805_read(dev, 0xc1, &cts_mid);
	if (!ret)
		ret = gc573_it6805_read(dev, 0xc2, &cts_high);

	cleanup_ret = gc573_it6805_set_bank(dev, GC573_IT6805_BANK_0);
	if (!cleanup_ret)
		bank0_known = true;
	else
		dev_warn(&dev->pdev->dev,
			 "audio clock: bank 0 restore after N/CTS read failed (%d)\n",
			 cleanup_ret);
	if (!ret)
		ret = cleanup_ret;

	if (bank0_known) {
		cleanup_ret = gc573_it6805_update_bits(dev, 0x86, BIT(0),
						       original_latch & BIT(0));
		if (cleanup_ret)
			dev_warn(&dev->pdev->dev,
				 "audio clock: 0x86 latch restore failed (%d)\n",
				 cleanup_ret);
		if (!ret)
			ret = cleanup_ret;
	} else {
		/* Bank state unknown; leave the latch as-is and report. */
		dev_warn(&dev->pdev->dev,
			 "audio clock: 0x86 latch restore skipped (bank unknown)\n");
	}
	if (ret)
		return ret;

	*n = ((u32)n_high << 12) | ((u32)n_mid << 4) | (n_cts_shared & 0x0f);
	*cts = ((u32)cts_mid << 12) | ((u32)cts_high << 4) | (cts_low >> 4);
	return 0;
}

/*
 * Caller holds dev->lock. Count audio TMDS edges against the stored OCLK
 * reference over 10 samples (reference it6805_measure_audio_tmds_locked).
 * The clock reconstruction uses the shared gc573_it6805_tmds_clock_khz so the
 * module and the offline unit test execute the identical math. Returns the
 * reconstructed audio TMDS (pixel) clock in kHz.
 */
static int gc573_it6805_measure_audio_tmds(struct gc573_device *dev,
					   u32 *clock_khz)
{
	u32 sample_sum = 0;
	u8 reg43;
	u8 sample;
	unsigned int i;
	int ret;

	if (!dev->oclk_reference_khz)
		return -ENODATA;

	ret = gc573_it6805_set_bank(dev, GC573_IT6805_BANK_0);
	for (i = 0; !ret && i < GC573_IT6805_AUDIO_TMDS_SAMPLES; i++) {
		msleep(GC573_IT6805_AUDIO_TMDS_DELAY_MS);
		ret = gc573_it6805_read(dev, 0x48, &sample);
		if (!ret)
			sample_sum += sample + 1U;
	}
	if (!ret)
		ret = gc573_it6805_read(dev, 0x43, &reg43);
	if (ret)
		return ret;
	if (!sample_sum)
		return -EAGAIN;

	*clock_khz = gc573_it6805_tmds_clock_khz(dev->oclk_reference_khz,
						 reg43, sample_sum);
	return 0;
}

/*
 * Caller holds dev->lock. Pulse 0x22[1] then replay the current 0x8a byte four
 * times (reference it6805_reset_audio_logic_locked / vendor Reset_Audio_Logic).
 */
static int gc573_it6805_reset_audio_logic(struct gc573_device *dev)
{
	u8 reg8a;
	unsigned int i;
	int ret;

	ret = gc573_it6805_set_bank(dev, GC573_IT6805_BANK_0);
	if (!ret)
		ret = gc573_it6805_update_bits(dev, 0x22, BIT(1), BIT(1));
	if (!ret)
		ret = gc573_it6805_update_bits(dev, 0x22, BIT(1), 0);
	if (!ret)
		ret = gc573_it6805_read(dev, 0x8a, &reg8a);
	for (i = 0; !ret && i < 4; i++)
		ret = gc573_it6805_write(dev, 0x8a, reg8a);
	if (ret)
		gc573_it6805_set_bank(dev, GC573_IT6805_BANK_0);
	return ret;
}

/*
 * Caller holds dev->lock. Derive the computed sample-rate code from latched
 * N/CTS and a measured audio TMDS clock, decode the receiver's internal rate
 * code as (B6>>2 & 0x30) | (B5 & 0x0f), and force the 0x81[6]/0x8a[5:0]
 * override (reference it6805_update_audio_clock_locked / vendor
 * iTE6805_Enable_Audio_Output).
 *
 * The reference forces the override only after IT6805_AUDIO_RATE_MISMATCH_LIMIT
 * (16) consecutive confirmed mismatches across its runtime poll cycles; the
 * vendor does the same (counter forces at 0x10). This one-shot start op
 * replicates that stability gate inside a single call: re-measure and
 * re-compare up to 16 times, with fail-closed differences for a bounded start
 * path:
 *   - a poll with N=0, CTS=0 or no TMDS measurement is an invalid measurement
 *     (-ENODATA), not a success: the reference treats zero CTS/TMDS as "no
 *     rate to compute" and its caller still untristates, which would release
 *     the audio output with an unverified clock.
 *   - a computed rate outside the IT6805 rate table (the 1024 kHz sentinel)
 *     is rejected (-EOPNOTSUPP) rather than forced: the reference maps
 *     out-of-range rates to the sentinel code and would write it.
 *   - a mismatch only counts toward the limit when the computed code is
 *     stable (identical to the previous poll); a fluctuating rate resets the
 *     counter, so the override is applied to one stable, supported result.
 *   - the experimental build is stereo48k-only, so a stable mismatch at any
 *     computed code other than 48 kHz is refused rather than forced.
 *   - the video lock (bank-0 0x19 bit 7) is re-checked immediately before
 *     forcing; a link change mid-prepare aborts the override.
 *   - the override saves the prior 0x81/0x8a values and rolls both back on
 *     any partial failure, logging the rollback result; a failed rollback is
 *     reported as unknown receiver state, never papered over.
 * Every measurement is logged for the bench.
 */
static int gc573_receiver_update_audio_clock(struct gc573_device *dev)
{
	u32 tmds_khz, n, cts, rate_khz;
	u8 computed, internal, prev_code, reg81, reg8a, b5, b6, scdt19;
	unsigned int i, stable;
	int ret;

	prev_code = 0;
	stable = 0;
	for (i = 0; i < GC573_IT6805_AUDIO_RATE_MISMATCH_LIMIT; i++) {
		ret = gc573_it6805_read_ncts(dev, &n, &cts);
		if (ret)
			return ret;
		ret = gc573_it6805_measure_audio_tmds(dev, &tmds_khz);
		if (ret) {
			dev_warn(&dev->pdev->dev,
				 "audio clock: TMDS measure failed (%d); skipping rate update\n",
				 ret);
			return ret;
		}

		if (!n || !cts || !tmds_khz) {
			dev_warn(&dev->pdev->dev,
				 "audio clock: N=%u CTS=%u tmds=%u kHz; invalid measurement, failing closed\n",
				 n, cts, tmds_khz);
			return -ENODATA;
		}

		rate_khz = gc573_it6805_compute_rate_khz(tmds_khz, n, cts);
		computed = gc573_it6805_audio_rate_code(rate_khz);
		if (computed != GC573_IT6805_AUDIO_RATE_48_KHZ) {
			dev_warn(&dev->pdev->dev,
				 "audio clock: rate=%u kHz outside supported 48 kHz bin (N=%u CTS=%u tmds=%u kHz); refusing to force a rate\n",
				 rate_khz, n, cts, tmds_khz);
			return -EOPNOTSUPP;
		}

		ret = gc573_it6805_set_bank(dev, GC573_IT6805_BANK_0);
		if (!ret)
			ret = gc573_it6805_read(dev, 0xb5, &b5);
		if (!ret)
			ret = gc573_it6805_read(dev, 0xb6, &b6);
		if (ret)
			return ret;

		/* Vendor functional decode: B6 upper, B5 lower. */
		internal = gc573_it6805_decode_receiver_rate(b5, b6);
		if (computed == prev_code)
			stable++;
		else
			stable = 1;
		prev_code = computed;
		if (stable < GC573_IT6805_AUDIO_RATE_MISMATCH_LIMIT)
			continue;
		ret = gc573_it6805_read(dev, 0x19, &scdt19);
		if (ret)
			return ret;
		if (scdt19 == 0xff || !(scdt19 & BIT(7)))
			return -ENOLINK;
		if (internal == computed) {
			dev_info(&dev->pdev->dev,
				 "audio clock: N=%u CTS=%u tmds=%u kHz rate=%u kHz computed=0x%02x internal=0x%02x b5=%02x b6=%02x match (poll %u)\n",
				 n, cts, tmds_khz, rate_khz, computed, internal,
				 b5, b6, i + 1);
			ret = gc573_it6805_read(dev, 0x81, &reg81);
			if (!ret)
				ret = gc573_it6805_update_bits(dev, 0x81,
							      BIT(6), 0);
			if (!ret && (reg81 & BIT(6)))
				ret = gc573_it6805_reset_audio_logic(dev);
			if (ret)
				gc573_it6805_set_bank(dev, GC573_IT6805_BANK_0);
			return ret;
		}

		dev_info(&dev->pdev->dev,
			 "audio clock mismatch (poll %u): N=%u CTS=%u tmds=%u kHz rate=%u kHz computed=0x%02x internal=0x%02x b5=%02x b6=%02x\n",
			 i + 1, n, cts, tmds_khz, rate_khz, computed,
			 internal, b5, b6);
	}

	if (stable < GC573_IT6805_AUDIO_RATE_MISMATCH_LIMIT) {
		dev_info(&dev->pdev->dev,
			 "audio clock: mismatched polls did not stay stable (stable run %u of %u); no override\n",
			 stable, GC573_IT6805_AUDIO_RATE_MISMATCH_LIMIT);
		return -EAGAIN;
	}

	if (computed != GC573_IT6805_AUDIO_RATE_48_KHZ) {
		dev_warn(&dev->pdev->dev,
			 "audio clock: stable mismatch at computed code 0x%02x (non-48k); experimental build is stereo48k-only, refusing override\n",
			 computed);
		return -EOPNOTSUPP;
	}

	/* Continuing-link check before forcing: bank-0 0x19 bit 7. */
	ret = gc573_it6805_read(dev, 0x19, &scdt19);
	if (ret)
		return ret;
	if (scdt19 == 0xff || !(scdt19 & BIT(7))) {
		dev_warn(&dev->pdev->dev,
			 "audio clock: video lock lost before override; aborting\n");
		return -ENOLINK;
	}

	/* Save prior state for rollback on a partial failure. */
	ret = gc573_it6805_read(dev, 0x81, &reg81);
	if (!ret)
		ret = gc573_it6805_read(dev, 0x8a, &reg8a);
	if (ret)
		return ret;

	ret = gc573_it6805_update_bits(dev, 0x81, BIT(6), BIT(6));
	if (!ret)
		ret = gc573_it6805_update_bits(dev, 0x8a, 0x3f, computed);
	if (!ret)
		ret = gc573_it6805_reset_audio_logic(dev);
	if (ret) {
		int rollback;

		/* Roll the partial override back and log the evidence. */
		rollback = gc573_it6805_update_bits(dev, 0x81, BIT(6),
						    reg81 & BIT(6));
		{
			int restore8a = gc573_it6805_update_bits(dev, 0x8a, 0x3f,
							       reg8a & 0x3f);
			if (!rollback)
				rollback = restore8a;
		}
		dev_err(&dev->pdev->dev,
			"audio clock: override failed (%d); rolled 0x81/0x8a back to 0x%02x/0x%02x (rollback %d)\n",
			ret, reg81, reg8a, rollback);
		if (rollback)
			dev_err(&dev->pdev->dev,
				"audio clock: rollback also failed (%d); receiver rate state unknown\n",
				rollback);
		gc573_it6805_set_bank(dev, GC573_IT6805_BANK_0);
		return ret;
	}

	dev_info(&dev->pdev->dev,
		 "audio clock: %u consecutive stable mismatches; forced override 0x81[6]=1 0x8a[5:0]=0x%02x (48 kHz), audio logic reset\n",
		 GC573_IT6805_AUDIO_RATE_MISMATCH_LIMIT, computed);
	return 0;
}

/*
 * Experimental audio bring-up (caller holds dev->vdev->lock; takes dev->lock
 * internally). Order follows reference it6805_start_audio_output_locked:
 * request, wait ready, refresh format, reset, update clock (rate override),
 * untristate.
 */
int gc573_receiver_prepare_audio(struct gc573_device *dev)
{
	struct gc573_audio_status status;
	unsigned int i;
	int cleanup_ret;
	int ret;

	/* Caller-held vdev->lock covers both snapshot and subsequent writes. */
	ret = gc573_receiver_audio_snapshot(dev, &status);
	if (ret)
		return ret;
	if (!status.video_scdt || status.receiver_scdt_19 == 0xff)
		return -ENOLINK;
	if (!status.receiver_reports_48k_lpcm_stereo)
		return -EOPNOTSUPP;

	/* Reference it6805_request_audio_locked: request before readiness/reset.
	 * This is bounded process-context bring-up, not the full runtime FSM.
	 * On failure the reference unconditionally re-clears 0x8c[4] and bank 0
	 * (ignoring the cleanup's own result); mirrored here. */
	mutex_lock(&dev->lock);
	ret = gc573_it6805_set_bank(dev, GC573_IT6805_BANK_0);
	if (!ret) {
		ret = gc573_it6805_update_bits(dev, 0x8c, BIT(4), BIT(4));
		cleanup_ret = gc573_it6805_update_bits(dev, 0x8c, BIT(4), 0);
		if (!ret)
			ret = cleanup_ret;
	}
	if (ret) {
		cleanup_ret = gc573_it6805_set_bank(dev, GC573_IT6805_BANK_0);
		if (!cleanup_ret)
			cleanup_ret = gc573_it6805_update_bits(dev, 0x8c, BIT(4), 0);
		if (cleanup_ret)
			dev_warn(&dev->pdev->dev, "audio request cleanup failed: %d\n", cleanup_ret);
		mutex_unlock(&dev->lock);
		return ret;
	}
	cleanup_ret = gc573_it6805_set_bank(dev, GC573_IT6805_BANK_0);
	if (!ret)
		ret = cleanup_ret;
	mutex_unlock(&dev->lock);
	if (ret)
		return ret;
	for (i = 0; i < 20; i++) {
		msleep(50);
		ret = gc573_receiver_audio_snapshot(dev, &status);
		if (ret)
			return ret;
		if (status.video_scdt && status.receiver_reports_48k_lpcm_stereo)
			break;
	}
	if (i == 20)
		return -ETIMEDOUT;

	mutex_lock(&dev->lock);
	ret = gc573_it6805_set_bank(dev, GC573_IT6805_BANK_0);
	if (!ret)
		ret = gc573_it6805_update_bits(dev, 0x8c, BIT(3),
					      status.infoframe_b2 & BIT(1) ? BIT(3) : 0);
	if (!ret)
		ret = gc573_it6805_reset_audio_logic(dev);
	if (!ret)
		ret = gc573_receiver_update_audio_clock(dev);
	if (!ret)
		ret = gc573_it6805_set_bank(dev, GC573_IT6805_BANK_1);
	if (!ret)
		ret = gc573_it6805_write(dev, 0xc7, 0x00);
	if (ret) {
		/* Reference it6805_start_audio_output_locked: any failure
		 * clears the audio output (clear 0x81[6], re-tristate
		 * 0xc7=0x7f) before returning. */
		int clear_ret = gc573_it6805_set_bank(dev, GC573_IT6805_BANK_0);
		if (!clear_ret)
			clear_ret = gc573_it6805_update_bits(dev, 0x81, BIT(6), 0);
		cleanup_ret = gc573_it6805_set_bank(dev, GC573_IT6805_BANK_1);
		if (!cleanup_ret)
			cleanup_ret = gc573_it6805_write(dev, 0xc7, 0x7f);
		if (!clear_ret)
			clear_ret = cleanup_ret;
		cleanup_ret = gc573_it6805_set_bank(dev, GC573_IT6805_BANK_0);
		if (!clear_ret)
			clear_ret = cleanup_ret;
		dev_warn(&dev->pdev->dev,
			 "audio bring-up failed (%d); output cleanup result=%d\n",
			 ret, clear_ret);
		mutex_unlock(&dev->lock);
		return ret;
	}
	cleanup_ret = gc573_it6805_set_bank(dev, GC573_IT6805_BANK_0);
	if (!ret)
		ret = cleanup_ret;
	mutex_unlock(&dev->lock);
	return ret;
}

static int gc573_it6805_configure_capture(struct gc573_device *dev,
						  u32 width, u32 height,
						  u32 fourcc)
{
	u8 timing[9];
	u32 input_width, input_height;
	int ret;

	if (!dev || !width || !height)
		return -EINVAL;
	if (fourcc != V4L2_PIX_FMT_YUYV)
		return -EOPNOTSUPP;

	ret = gc573_it6805_wait_for_lock(dev);
	if (ret) {
		dev_warn(&dev->pdev->dev,
			 "capture refused: receiver lock not confirmed within 10 seconds (%d)\n",
			 ret);
		return ret;
	}

	/* Bank 0 active timing fields, also read by the reference receiver. */
	ret = gc573_i2c_read_block(dev, GC573_IT6805_ADDRESS, 0x9d,
				  timing, sizeof(timing));
	if (ret)
		return ret;
	input_width = (timing[0] | (timing[1] << 8)) & 0x3fff;
	input_height = (timing[7] | (timing[8] << 8)) & 0x3fff;
	dev_info(&dev->pdev->dev, "receiver active timing %ux%u; requested %ux%u\n",
		 input_width, input_height, width, height);
	if (input_width != width || input_height != height)
		return -EOPNOTSUPP;
	if (!preserve_receiver) {
		ret = gc573_it6805_configure_output(dev);
		if (ret)
			dev_warn(&dev->pdev->dev,
				 "receiver output configuration failed: %d\n", ret);
		else {
			bool locked;

			/* Bring-up settling interval: 300 ms still yielded black first
			 * frames on this source. Recheck lock before starting DMA. */
			msleep(1000);
			ret = gc573_it6805_status(dev, &locked);
			if (!ret && !locked)
				ret = gc573_it6805_wait_for_lock(dev);
		}
		return ret;
	}

	/* Stream-on is the point at which the FPGA begins sampling TTL output. */
	return gc573_it6805_set_video_tristate(dev, false);
}

const struct gc573_receiver_ops gc573_it6805_source = {
	.identify = gc573_it6805_identify,
	.initialize = gc573_it6805_initialize,
	.get_signal_status = gc573_it6805_status,
	.configure_capture = gc573_it6805_configure_capture,
};

static int gc573_receiver_identify_unavailable(struct gc573_device *dev)
{
	return -EOPNOTSUPP;
}

static int gc573_receiver_initialize_unavailable(struct gc573_device *dev)
{
	return -EOPNOTSUPP;
}

static int gc573_receiver_status_unavailable(struct gc573_device *dev,
						     bool *locked)
{
	return -EOPNOTSUPP;
}

static int gc573_receiver_configure_unavailable(struct gc573_device *dev,
							u32 width, u32 height,
							u32 fourcc)
{
	return -EOPNOTSUPP;
}

const struct gc573_receiver_ops gc573_it6664_unimplemented = {
	.identify = gc573_receiver_identify_unavailable,
	.initialize = gc573_receiver_initialize_unavailable,
	.get_signal_status = gc573_receiver_status_unavailable,
	.configure_capture = gc573_receiver_configure_unavailable,
};

static int gc573_irq_status_unavailable(struct gc573_device *dev, u32 *pending)
{
	return -EOPNOTSUPP;
}

const struct gc573_irq_ops gc573_irq_unimplemented = {
	.read_and_ack = gc573_irq_status_unavailable,
};

irqreturn_t gc573_irq_handler(int irq, void *opaque)
{
	struct gc573_device *dev = opaque;
	void *cookie;
	irqreturn_t ret;

	if (!dev)
		return IRQ_NONE;

	ret = gc573_hw_irq(dev, &cookie);
	if (ret == IRQ_HANDLED && cookie)
		gc573_v4l2_buffer_done(dev, cookie);
	return ret;
}
