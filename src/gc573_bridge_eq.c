// SPDX-License-Identifier: GPL-2.0-only
/*
 * Synchronous bounded IT6664 HDMI 2.0 RX EQ calibration.
 * Ported from gc555-it6664-core.c: it6664_set_rx_sareq(),
 * it6664_start_rx_eq20(), it6664_measure_rx_eq20(),
 * it6664_score_rx_eq20_lane(), and it6664_finish_rx_eq20_result().
 */
#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/types.h>

#include "gc573_pure.h"
#include "gc573_bridge_eq.h"

#define GC573_EQ_SWITCH_ADDR             0x58
#define GC573_EQ_RX_ADDR                 0x70

#define GC573_EQ_REG_BANK                0x0f
#define GC573_EQ_REG_IRQ_COMMON          0x05
#define GC573_EQ_REG_STATUS              0x13
#define GC573_EQ_REG_STATUS_14           0x14
#define GC573_EQ_REG_SCDT                0x19
#define GC573_EQ_BANK_MASK               GENMASK(1, 0)
#define GC573_EQ_BANK_0                  0x00
#define GC573_EQ_BANK_3                  0x03

#define GC573_EQ_SWITCH_IRQ_RX           BIT(4)
#define GC573_EQ_IRQ05_SUPPORTED         (BIT(0) | BIT(6) | BIT(4) | \
					 BIT(2) | BIT(1))
#define GC573_EQ_IRQ06_SUPPORTED         (BIT(7) | BIT(6) | BIT(0))
#define GC573_EQ_IRQ07_RESULT             (BIT(7) | BIT(6) | BIT(4))
#define GC573_EQ_IRQ07_BANK2_STATUS       BIT(2)
#define GC573_EQ_IRQ07_SUPPORTED          (GC573_EQ_IRQ07_RESULT | \
					 GC573_EQ_IRQ07_BANK2_STATUS)
#define GC573_EQ_IRQ08_SUPPORTED          (BIT(6) | BIT(2))
#define GC573_EQ_IRQ10_SUPPORTED          GENMASK(3, 0)
#define GC573_EQ_IRQ11_SUPPORTED          (GENMASK(6, 4) | BIT(3) | BIT(2))
#define GC573_EQ_IRQ12_SUPPORTED          (BIT(7) | BIT(5) | BIT(0))

#define GC573_EQ20_SNAPSHOT_SIZE         9
#define GC573_EQ20_CANDIDATE_COUNT       14
#define GC573_EQ20_TUNING_SIZE           3
#define GC573_EQ_LANE_COUNT              3
#define GC573_EQ20_TIMEOUT_MS            5000
#define GC573_EQ20_POLL_MS               10

enum gc573_eq20_path {
	GC573_EQ20_PATH_NONE,
	GC573_EQ20_PATH_INVALID,
	GC573_EQ20_PATH_RESTORE_SNAPSHOT,
	GC573_EQ20_PATH_SCORE_AMP,
};

struct gc573_eq20_context {
	enum gc573_eq20_path path;
	unsigned long deadline;
	u16 invalid_mask[GC573_EQ_LANE_COUNT];
	u8 seed[GC573_EQ_LANE_COUNT];
	u8 snapshot[GC573_EQ20_SNAPSHOT_SIZE];
	u8 readback[GC573_EQ20_SNAPSHOT_SIZE];
	u8 tuning[GC573_EQ20_CANDIDATE_COUNT]
		 [GC573_EQ_LANE_COUNT][GC573_EQ20_TUNING_SIZE];
};

struct gc573_eq20_irq {
	u8 reg05;
	u8 reg06;
	u8 reg07;
	u8 reg08;
	u8 reg09;
	u8 reg10;
	u8 reg11;
	u8 reg12;
	u8 reg13;
	u8 reg14;
	u8 reg19;
};

static int gc573_eq_read(struct gc573_device *dev, u8 address, u8 reg,
			 u8 *value)
{
	return gc573_i2c_read_reg(dev, address, reg, value);
}

static int gc573_eq_write(struct gc573_device *dev, u8 address, u8 reg,
			  u8 value)
{
	return gc573_i2c_write_reg(dev, address, reg, value);
}

static int gc573_eq_update(struct gc573_device *dev, u8 address, u8 reg,
			   u8 mask, u8 value)
{
	u8 old_value;
	int ret;

	ret = gc573_eq_read(dev, address, reg, &old_value);
	if (ret)
		return ret;
	return gc573_eq_write(dev, address, reg,
			      (old_value & ~mask) | (value & mask));
}

static int gc573_eq_select_bank(struct gc573_device *dev, u8 bank)
{
	return gc573_eq_update(dev, GC573_EQ_RX_ADDR, GC573_EQ_REG_BANK,
			       GC573_EQ_BANK_MASK, bank);
}

static int gc573_eq_set_sareq(struct gc573_device *dev, u8 parameter)
{
	bool bank_three = false;
	int cleanup_ret;
	int ret;

	ret = gc573_eq_select_bank(dev, GC573_EQ_BANK_3);
	if (ret)
		return ret;
	bank_three = true;
	ret = gc573_eq_update(dev, GC573_EQ_RX_ADDR, 0x20, BIT(7), 0);
	if (ret)
		goto cleanup;
	ret = gc573_eq_write(dev, GC573_EQ_RX_ADDR, 0x22, 0x00);
	if (ret)
		goto cleanup;
	ret = gc573_eq_select_bank(dev, GC573_EQ_BANK_0);
	if (ret)
		goto cleanup;
	bank_three = false;
	ret = gc573_eq_write(dev, GC573_EQ_RX_ADDR, 0x07, 0xff);
	if (ret)
		return ret;
	ret = gc573_eq_write(dev, GC573_EQ_RX_ADDR, 0x23, 0xb0);
	if (ret)
		return ret;
	ret = gc573_eq_write(dev, GC573_EQ_RX_ADDR, 0x23, 0xa0);
	if (ret)
		return ret;
	ret = gc573_eq_update(dev, GC573_EQ_RX_ADDR, 0x3b, GENMASK(2, 0),
			      0x03);
	if (ret)
		return ret;
	ret = gc573_eq_select_bank(dev, GC573_EQ_BANK_3);
	if (ret)
		return ret;
	bank_three = true;
	ret = gc573_eq_write(dev, GC573_EQ_RX_ADDR, 0x26, 0x00);
	if (ret)
		goto cleanup;
	ret = gc573_eq_write(dev, GC573_EQ_RX_ADDR, 0x27, 0x1f);
	if (ret)
		goto cleanup;
	ret = gc573_eq_write(dev, GC573_EQ_RX_ADDR, 0x28, 0x1f);
	if (ret)
		goto cleanup;
	ret = gc573_eq_write(dev, GC573_EQ_RX_ADDR, 0x29, 0x1f);
	if (ret)
		goto cleanup;
	ret = gc573_eq_update(dev, GC573_EQ_RX_ADDR, 0x2d, GENMASK(2, 0), 0);
	if (ret)
		goto cleanup;
	ret = gc573_eq_write(dev, GC573_EQ_RX_ADDR, 0x30,
			     (parameter << 2) ^ BIT(7));
	if (ret)
		goto cleanup;
	ret = gc573_eq_write(dev, GC573_EQ_RX_ADDR, 0x31, 0xb0);
	if (ret)
		goto cleanup;
	ret = gc573_eq_write(dev, GC573_EQ_RX_ADDR, 0x32, 0x43);
	if (ret)
		goto cleanup;
	ret = gc573_eq_write(dev, GC573_EQ_RX_ADDR, 0x33, 0x47);
	if (ret)
		goto cleanup;
	ret = gc573_eq_write(dev, GC573_EQ_RX_ADDR, 0x34, 0x4b);
	if (ret)
		goto cleanup;
	ret = gc573_eq_write(dev, GC573_EQ_RX_ADDR, 0x35, 0x53);
	if (ret)
		goto cleanup;
	ret = gc573_eq_update(dev, GC573_EQ_RX_ADDR, 0x36, GENMASK(7, 6), 0);
	if (ret)
		goto cleanup;
	ret = gc573_eq_write(dev, GC573_EQ_RX_ADDR, 0x37, 0x0b);
	if (ret)
		goto cleanup;
	ret = gc573_eq_write(dev, GC573_EQ_RX_ADDR, 0x38, 0xf2);
	if (ret)
		goto cleanup;
	ret = gc573_eq_write(dev, GC573_EQ_RX_ADDR, 0x39, 0x0d);
	if (ret)
		goto cleanup;
	ret = gc573_eq_update(dev, GC573_EQ_RX_ADDR, 0x4a, BIT(7), 0);
	if (ret)
		goto cleanup;
	ret = gc573_eq_update(dev, GC573_EQ_RX_ADDR, 0x4b, BIT(7), 0);
	if (ret)
		goto cleanup;
	ret = gc573_eq_update(dev, GC573_EQ_RX_ADDR, 0x54, BIT(7), BIT(7));
	if (ret)
		goto cleanup;
	ret = gc573_eq_update(dev, GC573_EQ_RX_ADDR, 0x54, GENMASK(5, 3),
			      GENMASK(5, 3));
	if (ret)
		goto cleanup;
	ret = gc573_eq_write(dev, GC573_EQ_RX_ADDR, 0x55, 0x40);
	if (ret)
		goto cleanup;
	ret = gc573_eq_update(dev, GC573_EQ_RX_ADDR, 0x22, BIT(2), BIT(2));

cleanup:
	if (bank_three) {
		cleanup_ret = gc573_eq_select_bank(dev, GC573_EQ_BANK_0);
		if (!ret)
			ret = cleanup_ret;
	}
	return ret;
}

static int gc573_eq20_start(struct gc573_device *dev, u8 status14,
			    struct gc573_eq20_context *context)
{
	int ret;

	ret = gc573_eq_update(dev, GC573_EQ_RX_ADDR, 0x53, BIT(5), 0);
	if (ret)
		return ret;
	ret = gc573_eq_update(dev, GC573_EQ_RX_ADDR, 0x05, BIT(5), BIT(5));
	if (ret)
		return ret;
	if (status14 & BIT(7)) {
		ret = gc573_eq_set_sareq(dev, 0);
		if (ret)
			return ret;
	}
	context->path = GC573_EQ20_PATH_NONE;
	return 0;
}

static int gc573_eq20_measure(struct gc573_device *dev,
			      struct gc573_eq20_context *context)
{
	u8 amp_status[GC573_EQ_LANE_COUNT];
	u8 invalid_lo;
	u8 invalid_hi;
	u8 reg4a = 0;
	u8 reg37 = 0;
	u8 ignored;
	bool have_reg4a = false;
	bool have_reg37 = false;
	bool bank_three = false;
	unsigned int lane;
	int cleanup_ret;
	int ret;

	ret = gc573_eq_select_bank(dev, GC573_EQ_BANK_3);
	if (ret)
		return ret;
	bank_three = true;
	ret = gc573_eq_read(dev, GC573_EQ_RX_ADDR, 0x4a, &reg4a);
	if (ret)
		goto cleanup;
	have_reg4a = true;
	ret = gc573_eq_update(dev, GC573_EQ_RX_ADDR, 0x4a, BIT(7), BIT(7));
	if (ret)
		goto cleanup;
	for (lane = 0; lane < ARRAY_SIZE(context->snapshot); lane++) {
		if (time_after_eq(jiffies, context->deadline)) {
			ret = -ETIMEDOUT;
			goto cleanup;
		}
		ret = gc573_eq_read(dev, GC573_EQ_RX_ADDR, 0x4b + lane,
				    &context->snapshot[lane]);
		if (ret)
			goto cleanup;
	}
	ret = gc573_eq_update(dev, GC573_EQ_RX_ADDR, 0x4a, BIT(7),
			      reg4a & BIT(7));
	if (ret)
		goto cleanup;
	ret = gc573_eq_read(dev, GC573_EQ_RX_ADDR, 0x37, &reg37);
	if (ret)
		goto cleanup;
	have_reg37 = true;
	for (lane = 0; lane < GC573_EQ_LANE_COUNT; lane++) {
		if (time_after_eq(jiffies, context->deadline)) {
			ret = -ETIMEDOUT;
			goto cleanup;
		}
		ret = gc573_eq_update(dev, GC573_EQ_RX_ADDR, 0x37,
				      GENMASK(7, 6), lane << 6);
		if (ret)
			goto cleanup;
		ret = gc573_eq_read(dev, GC573_EQ_RX_ADDR, 0x63, &invalid_lo);
		if (ret)
			goto cleanup;
		ret = gc573_eq_read(dev, GC573_EQ_RX_ADDR, 0x64, &invalid_hi);
		if (ret)
			goto cleanup;
		ret = gc573_eq_read(dev, GC573_EQ_RX_ADDR, 0x6d,
				    &amp_status[lane]);
		if (ret)
			goto cleanup;
		ret = gc573_eq_read(dev, GC573_EQ_RX_ADDR, 0x6e, &ignored);
		if (ret)
			goto cleanup;
		context->invalid_mask[lane] = invalid_lo | (invalid_hi << 8);
	}
	ret = gc573_eq_update(dev, GC573_EQ_RX_ADDR, 0x37, GENMASK(7, 6),
			      reg37 & GENMASK(7, 6));
	if (ret)
		goto cleanup;
	ret = gc573_eq_select_bank(dev, GC573_EQ_BANK_0);
	if (ret)
		goto cleanup;
	bank_three = false;

	if (context->invalid_mask[0] == 0x3fff ||
	    context->invalid_mask[1] == 0x3fff ||
	    context->invalid_mask[2] == 0x3fff)
		context->path = GC573_EQ20_PATH_INVALID;
	else if (amp_status[0] && amp_status[1] && amp_status[2])
		context->path = GC573_EQ20_PATH_RESTORE_SNAPSHOT;
	else
		context->path = GC573_EQ20_PATH_SCORE_AMP;
	return 0;

cleanup:
	if (bank_three) {
		if (have_reg37) {
			cleanup_ret = gc573_eq_update(dev, GC573_EQ_RX_ADDR, 0x37,
						      GENMASK(7, 6), reg37);
			if (!ret)
				ret = cleanup_ret;
		}
		if (have_reg4a) {
			cleanup_ret = gc573_eq_update(dev, GC573_EQ_RX_ADDR, 0x4a,
						      BIT(7), reg4a);
			if (!ret)
				ret = cleanup_ret;
		}
		cleanup_ret = gc573_eq_select_bank(dev, GC573_EQ_BANK_0);
		if (!ret)
			ret = cleanup_ret;
	}
	return ret;
}

static u8 gc573_eq_abs_diff_half(u8 left, u8 right)
{
	return left >= right ? (left - right) >> 1 : (right - left) >> 1;
}

static int gc573_eq20_score_lane(struct gc573_device *dev,
				 struct gc573_eq20_context *context,
				 unsigned int lane)
{
	static const u8 candidate_fixed[GC573_EQ20_CANDIDATE_COUNT] = {
		0x7f, 0x7e, 0x3f, 0x3e, 0x1f, 0x1e, 0x0f,
		0x0e, 0x07, 0x06, 0x03, 0x02, 0x01, 0x00,
	};
	u8 best_tuning[GC573_EQ20_TUNING_SIZE] = {};
	u8 best_primary = 0xff;
	u8 best_secondary = 0xff;
	u8 selected_raw = 0xff;
	u8 selected_index = 0xff;
	u8 center;
	u8 sample_a;
	u8 sample_b;
	u8 sample_c;
	u8 sum_ab;
	u8 sum_abc;
	u8 center_x2;
	u8 center_x3;
	u8 primary;
	u8 secondary;
	u8 delta_a;
	u8 delta_b;
	u8 delta_c;
	u8 ignored;
	u16 invalid_mask;
	unsigned int candidate;
	unsigned int i;
	int ret;

	if (lane >= GC573_EQ_LANE_COUNT)
		return -EINVAL;
	invalid_mask = context->invalid_mask[lane];
	ret = gc573_eq_update(dev, GC573_EQ_RX_ADDR, 0x37, GENMASK(7, 6),
			      lane << 6);
	if (ret)
		return ret;
	ret = gc573_eq_read(dev, GC573_EQ_RX_ADDR, 0xd5 + lane, &ignored);
	if (ret)
		return ret;

	for (candidate = 0; candidate < ARRAY_SIZE(candidate_fixed); candidate++) {
		if (time_after_eq(jiffies, context->deadline))
			return -ETIMEDOUT;
		ret = gc573_eq_update(dev, GC573_EQ_RX_ADDR, 0x36,
				      GENMASK(3, 0), candidate);
		if (ret)
			return ret;
		ret = gc573_eq_read(dev, GC573_EQ_RX_ADDR, 0x5d, &center);
		if (ret)
			return ret;
		ret = gc573_eq_read(dev, GC573_EQ_RX_ADDR, 0x5e, &sample_a);
		if (ret)
			return ret;
		ret = gc573_eq_read(dev, GC573_EQ_RX_ADDR, 0x5f, &sample_b);
		if (ret)
			return ret;
		ret = gc573_eq_read(dev, GC573_EQ_RX_ADDR, 0x60, &sample_c);
		if (ret)
			return ret;

		sum_ab = sample_a + sample_b;
		sum_abc = sum_ab + sample_c;
		center_x2 = center * 2;
		center_x3 = center * 3;
		primary = gc573_eq_abs_diff_half(sum_ab, center_x2);
		secondary = gc573_eq_abs_diff_half(sum_abc, center_x3);
		delta_a = gc573_eq_abs_diff_half(sample_a, center);
		delta_b = gc573_eq_abs_diff_half(sample_b, center);
		delta_c = gc573_eq_abs_diff_half(sample_c, center);
		if (delta_a & 0xe0)
			delta_a = 0x1f;
		/* The reference intentionally applies both overflow clamps to A. */
		if (delta_b & 0xf0)
			delta_a = 0x0f;
		if (delta_c & 0xf8)
			delta_a = 0x07;

		if (!(invalid_mask & BIT(0)) &&
		    (primary < best_primary ||
		     (primary == best_primary && secondary <= best_secondary))) {
			selected_raw = candidate_fixed[candidate];
			selected_index = candidate;
			best_primary = primary;
			best_secondary = secondary;
			best_tuning[0] = 0x40 + delta_a +
				(sample_a < center ? 0x20 : 0);
			best_tuning[1] = 0x20 + delta_b +
				(sample_b < center ? 0x10 : 0);
			best_tuning[2] = 0x10 + delta_c +
				(sample_c < center ? 0x08 : 0);
		}
		memcpy(context->tuning[candidate][lane], best_tuning,
		       sizeof(best_tuning));
		/* Preserve the reference's 7-bit mask after each candidate shift. */
		invalid_mask = (invalid_mask >> 1) & 0x7f;
	}
	if (selected_index == 0xff)
		return -EAGAIN;
	if (time_after_eq(jiffies, context->deadline))
		return -ETIMEDOUT;
	for (i = 0x61; i <= 0x62; i++) {
		ret = gc573_eq_read(dev, GC573_EQ_RX_ADDR, i, &ignored);
		if (ret)
			return ret;
	}
	for (i = 0x6b; i <= 0x6c; i++) {
		ret = gc573_eq_read(dev, GC573_EQ_RX_ADDR, i, &ignored);
		if (ret)
			return ret;
	}
	context->seed[lane] = selected_raw ^ BIT(7);
	ret = gc573_eq_write(dev, GC573_EQ_RX_ADDR, 0x27 + lane,
			     context->seed[lane]);
	if (ret)
		return ret;
	for (i = 0; i < ARRAY_SIZE(best_tuning); i++) {
		ret = gc573_eq_write(dev, GC573_EQ_RX_ADDR,
				     0x4b + lane * GC573_EQ20_TUNING_SIZE + i,
				     best_tuning[i]);
		if (ret)
			return ret;
	}
	ret = gc573_eq_update(dev, GC573_EQ_RX_ADDR, 0x4b, BIT(7), BIT(7));
	if (ret)
		return ret;
	ret = gc573_eq_update(dev, GC573_EQ_RX_ADDR, 0x4a, BIT(7), BIT(7));
	if (ret)
		return ret;
	return gc573_eq_update(dev, GC573_EQ_RX_ADDR, 0x4a, BIT(7), 0);
}

static int gc573_eq20_finish(struct gc573_device *dev,
			     struct gc573_eq20_context *context)
{
	static const u8 restore_masks[GC573_EQ20_SNAPSHOT_SIZE] = {
		0x7f, 0x3f, 0x1f, 0x7f, 0x3f, 0x1f, 0x7f, 0x3f, 0x1f,
	};
	u8 value;
	bool bank_three = false;
	unsigned int i;
	int cleanup_ret;
	int ret;

	if (context->path == GC573_EQ20_PATH_INVALID)
		return -EAGAIN;
	ret = gc573_eq_select_bank(dev, GC573_EQ_BANK_3);
	if (ret)
		return ret;
	bank_three = true;
	for (i = 0; i < ARRAY_SIZE(context->seed); i++) {
		if (time_after_eq(jiffies, context->deadline)) {
			ret = -ETIMEDOUT;
			goto cleanup;
		}
		ret = gc573_eq_read(dev, GC573_EQ_RX_ADDR, 0xd5 + i, &value);
		if (ret)
			goto cleanup;
		if (i == GC573_EQ_LANE_COUNT - 1)
			context->seed[i] = (value & 0x7f) | BIT(7);
		else
			context->seed[i] = (value & 0x7f) ^ BIT(7);
	}
	if (context->path == GC573_EQ20_PATH_RESTORE_SNAPSHOT) {
		ret = gc573_eq_update(dev, GC573_EQ_RX_ADDR, 0x4a, BIT(7), 0);
		if (ret)
			goto cleanup;
		ret = gc573_eq_update(dev, GC573_EQ_RX_ADDR, 0x37,
				      GENMASK(7, 6), BIT(7));
		if (ret)
			goto cleanup;
		for (i = 0; i < ARRAY_SIZE(restore_masks); i++) {
			if (time_after_eq(jiffies, context->deadline)) {
				ret = -ETIMEDOUT;
				goto cleanup;
			}
			ret = gc573_eq_update(dev, GC573_EQ_RX_ADDR, 0x4b + i,
					      restore_masks[i], context->snapshot[i]);
			if (ret)
				goto cleanup;
		}
		ret = gc573_eq_update(dev, GC573_EQ_RX_ADDR, 0x4b, BIT(7), BIT(7));
		if (ret)
			goto cleanup;
	} else if (context->path == GC573_EQ20_PATH_SCORE_AMP) {
		ret = gc573_eq_update(dev, GC573_EQ_RX_ADDR, 0x4a, BIT(7), 0);
		if (ret)
			goto cleanup;
		for (i = 0; i < GC573_EQ_LANE_COUNT; i++) {
			if (time_after_eq(jiffies, context->deadline)) {
				ret = -ETIMEDOUT;
				goto cleanup;
			}
			ret = gc573_eq20_score_lane(dev, context, i);
			if (ret)
				goto cleanup;
		}
	} else {
		ret = -EAGAIN;
		goto cleanup;
	}
	ret = gc573_eq_update(dev, GC573_EQ_RX_ADDR, 0x22, 0x44, BIT(6));
	if (ret)
		goto cleanup;
	ret = gc573_eq_select_bank(dev, GC573_EQ_BANK_0);
	if (ret)
		goto cleanup;
	bank_three = false;
	ret = gc573_eq_update(dev, GC573_EQ_RX_ADDR, 0x07,
			      GENMASK(5, 4), GENMASK(5, 4));
	if (ret)
		return ret;
	ret = gc573_eq_select_bank(dev, GC573_EQ_BANK_3);
	if (ret)
		return ret;
	bank_three = true;
	for (i = 0; i < ARRAY_SIZE(context->seed); i++) {
		if (time_after_eq(jiffies, context->deadline)) {
			ret = -ETIMEDOUT;
			goto cleanup;
		}
		ret = gc573_eq_write(dev, GC573_EQ_RX_ADDR, 0x27 + i,
				     context->seed[i]);
		if (ret)
			goto cleanup;
	}
	ret = gc573_eq_update(dev, GC573_EQ_RX_ADDR, 0x22, BIT(6), BIT(6));
	if (ret)
		goto cleanup;
	for (i = 0; i < ARRAY_SIZE(context->readback); i++) {
		if (time_after_eq(jiffies, context->deadline)) {
			ret = -ETIMEDOUT;
			goto cleanup;
		}
		ret = gc573_eq_read(dev, GC573_EQ_RX_ADDR, 0x4b + i,
				    &context->readback[i]);
		if (ret)
			goto cleanup;
	}
	ret = gc573_eq_write(dev, GC573_EQ_RX_ADDR, 0xe9, BIT(7));
	if (ret)
		goto cleanup;
	usleep_range(10000, 11000);
	ret = gc573_eq_write(dev, GC573_EQ_RX_ADDR, 0xe9, BIT(7));
	if (ret)
		goto cleanup;
	usleep_range(10000, 11000);
	ret = gc573_eq_select_bank(dev, GC573_EQ_BANK_0);
	if (ret)
		goto cleanup;
	bank_three = false;

	ret = gc573_eq_write(dev, GC573_EQ_SWITCH_ADDR, 0x0b, 0xff);
	if (ret)
		return ret;
	ret = gc573_eq_write(dev, GC573_EQ_SWITCH_ADDR, 0x0b, 0x00);
	if (ret)
		return ret;
	ret = gc573_eq_update(dev, GC573_EQ_SWITCH_ADDR, 0x4e,
			      GENMASK(3, 0), GENMASK(3, 0));
	if (ret)
		return ret;
	ret = gc573_eq_update(dev, GC573_EQ_SWITCH_ADDR, 0x0c, BIT(3), BIT(3));
	if (ret)
		return ret;
	ret = gc573_eq_update(dev, GC573_EQ_SWITCH_ADDR, 0x0c, BIT(3), 0);
	if (ret)
		return ret;
	ret = gc573_eq_update(dev, GC573_EQ_SWITCH_ADDR, 0x4e, GENMASK(3, 0), 0);
	if (ret)
		return ret;
	ret = gc573_eq_update(dev, GC573_EQ_RX_ADDR, 0x53, BIT(5), BIT(5));
	if (ret)
		return ret;
	return gc573_eq_update(dev, GC573_EQ_RX_ADDR, 0x05, BIT(5), BIT(5));

cleanup:
	if (bank_three) {
		cleanup_ret = gc573_eq_select_bank(dev, GC573_EQ_BANK_0);
		if (!ret)
			ret = cleanup_ret;
	}
	return ret;
}

static int gc573_eq20_read_irq(struct gc573_device *dev,
			       struct gc573_eq20_irq *irq)
{
	static const u8 regs[] = {
		0x05, 0x06, 0x07, 0x08, 0x09, 0x10, 0x11, 0x12,
		GC573_EQ_REG_STATUS, GC573_EQ_REG_STATUS_14, GC573_EQ_REG_SCDT,
	};
	u8 *values = (u8 *)irq;
	unsigned int i;
	int ret;

	for (i = 0; i < ARRAY_SIZE(regs); i++) {
		ret = gc573_eq_read(dev, GC573_EQ_RX_ADDR, regs[i], &values[i]);
		if (ret)
			return ret;
	}
	return 0;
}

static bool gc573_eq20_result_irq(const struct gc573_eq20_irq *irq)
{
	/* Probe owns EQ only: other latched events do not erase its result.
	 * Require the independently measured mature link for a coalesced event;
	 * leave unrelated IRQs pending for the future runtime implementation.
	 */
	return (irq->reg07 & GC573_EQ_IRQ07_RESULT) &&
	       (irq->reg13 & 0x99) == 0x99 && irq->reg13 != 0xff &&
	       (irq->reg14 & 0x38) == 0x38 &&
	       (irq->reg19 & BIT(7)) && irq->reg19 != 0xff;
}

static int gc573_eq20_ack_irq(struct gc573_device *dev,
			      const struct gc573_eq20_irq *irq)
{
	return gc573_eq_write(dev, GC573_EQ_RX_ADDR, 0x07,
			     irq->reg07 & GC573_EQ_IRQ07_SUPPORTED);
}

static int gc573_eq20_wait_result(struct gc573_device *dev,
				  unsigned long deadline,
				  struct gc573_eq20_irq *irq)
{
	u8 common;
	int ret;

	for (;;) {
		if (time_after_eq(jiffies, deadline))
			return -ETIMEDOUT;
		ret = gc573_eq_read(dev, GC573_EQ_SWITCH_ADDR,
				    GC573_EQ_REG_IRQ_COMMON, &common);
		if (ret)
			return ret;
		if (common == 0xff)
			return -ENODEV;
		if (common & GC573_EQ_SWITCH_IRQ_RX) {
			ret = gc573_eq20_read_irq(dev, irq);
			if (ret)
				return ret;
			if (time_after_eq(jiffies, deadline))
				return -ETIMEDOUT;
			if (gc573_eq20_result_irq(irq))
				return 0;
		}
		if (time_after_eq(jiffies, deadline))
			return -ETIMEDOUT;
		msleep(GC573_EQ20_POLL_MS);
	}
}

static int gc573_eq20_handle_result(struct gc573_device *dev,
				    struct gc573_eq20_context *context,
				    const struct gc573_eq20_irq *irq)
{
	int ret;

	if (time_after_eq(jiffies, context->deadline))
		return -ETIMEDOUT;

	if (irq->reg07 & GC573_EQ_IRQ07_BANK2_STATUS) {
		unsigned int i;
		static const u8 regs[] = { 0x24, 0x25, 0x26, 0x27 };
		u8 discard;
		int cleanup_ret;

		ret = gc573_eq_select_bank(dev, 0x02);
		if (ret)
			return ret;
		for (i = 0; i < ARRAY_SIZE(regs); i++) {
			ret = gc573_eq_read(dev, GC573_EQ_RX_ADDR, regs[i], &discard);
			if (ret)
				break;
		}
		cleanup_ret = gc573_eq_select_bank(dev, GC573_EQ_BANK_0);
		if (!ret)
			ret = cleanup_ret;
		if (ret)
			return ret;
	}
	if (irq->reg07 & BIT(7)) {
		ret = gc573_eq_select_bank(dev, GC573_EQ_BANK_3);
		if (ret)
			return ret;
		ret = gc573_eq_update(dev, GC573_EQ_RX_ADDR, 0x22, BIT(2), 0);
		if (!ret)
			ret = gc573_eq_update(dev, GC573_EQ_RX_ADDR, 0x22,
					      GENMASK(5, 3), 0);
		{
			int cleanup_ret = gc573_eq_select_bank(dev, GC573_EQ_BANK_0);

			if (!ret)
				ret = cleanup_ret;
		}
		if (ret)
			return ret;
	}
	ret = gc573_eq20_measure(dev, context);
	if (!ret && time_after_eq(jiffies, context->deadline))
		ret = -ETIMEDOUT;
	if (!ret)
		ret = gc573_eq20_finish(dev, context);
	if (!ret && time_after_eq(jiffies, context->deadline))
		ret = -ETIMEDOUT;
	if (!ret)
		ret = gc573_eq20_ack_irq(dev, irq);
	return ret;
}

int gc573_bridge_eq20_run(struct gc573_device *dev, u8 status14)
{
	struct gc573_eq20_context context = {};
	struct gc573_eq20_irq irq = {};
	unsigned long deadline;
	int ret;
	int cleanup_ret;

	if (!dev)
		return -EINVAL;
	deadline = jiffies + msecs_to_jiffies(GC573_EQ20_TIMEOUT_MS);
	context.deadline = deadline;
	if (!(status14 & BIT(6)))
		return -EINVAL;
	ret = gc573_eq_select_bank(dev, GC573_EQ_BANK_0);
	if (ret)
		return ret;
	ret = gc573_eq20_start(dev, status14, &context);
	if (ret)
		goto cleanup;
	if (time_after_eq(jiffies, deadline)) {
		ret = -ETIMEDOUT;
		goto cleanup;
	}
	ret = gc573_eq20_wait_result(dev, deadline, &irq);
	if (ret)
		goto cleanup;
	dev_info(&dev->pdev->dev, "IT6664 EQ20 result=%02x mature=%02x/%02x/%02x pending12=%02x\n",
		 irq.reg07, irq.reg13, irq.reg14, irq.reg19, irq.reg12);
	ret = gc573_eq20_handle_result(dev, &context, &irq);
	dev_info(&dev->pdev->dev, "IT6664 EQ20 finish=%d path=%u invalid=%04x/%04x/%04x\n",
		 ret, context.path, context.invalid_mask[0], context.invalid_mask[1], context.invalid_mask[2]);

cleanup:
	cleanup_ret = gc573_eq_select_bank(dev, GC573_EQ_BANK_0);
	return ret ? ret : cleanup_ret;
}
