// SPDX-License-Identifier: GPL-2.0-only
/* GC573 six-tap/four-pixel scaler. See maps/scaler-*-20260924.md.
 * Register order and calculations reproduce the pinned reference routines;
 * the module does not link or execute that object. */
#include <linux/errno.h>
#include <linux/kernel.h>
#include "gc573_pure.h"
#include "gc573_scaler.h"
#include "gc573_scaler_coeff.h"
#include "gc573_scaler_phase.h"

#define GC573_VSCALER 0x60000
#define GC573_HSCALER 0x40000
#define GC573_SCALER_PHASE_ENTRIES 1024

static int gc573_scaler_coefficients(struct gc573_device *dev, u32 base)
{
	unsigned int phase, pair;
	int ret;

	for (phase = 0; phase < 64; phase++) {
		for (pair = 0; pair < 3; pair++) {
			u32 packed = (u16)gc573_scaler_coeff[phase][pair * 2] |
				     ((u32)(u16)gc573_scaler_coeff[phase][pair * 2 + 1] << 16);

			ret = gc573_mmio_write32(&dev->mmio,
				base + 0x800 + (phase * 3 + pair) * 4, packed);
			if (ret)
				return ret;
		}
	}
	return 0;
}

int gc573_scaler_setup(struct gc573_device *dev, u32 input_width,
		       u32 input_height, u32 output_width, u32 output_height)
{
	u32 acc = 0, lane = 0, output_pixel = 0;
	u32 horizontal_step, vertical_step, entry;
	int ret;

	if (!dev ||
	    !(((input_width == 3840 && input_height == 2160) ||
	       (input_width == 1920 && input_height == 1080) ||
	       (input_width == 1280 &&
		(input_height == 720 || input_height == 800))) &&
	      ((output_width == 3840 && output_height == 2160) ||
	       (output_width == 1920 && output_height == 1080) ||
	       (output_width == 1280 &&
		(output_height == 720 || output_height == 800)))))
		return -EINVAL;
	horizontal_step = (input_width << 16) / output_width;
	vertical_step = (input_height << 16) / output_height;

	ret = gc573_scaler_coefficients(dev, GC573_VSCALER);
	if (!ret)
		ret = gc573_mmio_write32(&dev->mmio, GC573_VSCALER + 0x10, input_height);
	if (!ret)
		ret = gc573_mmio_write32(&dev->mmio, GC573_VSCALER + 0x18, input_width);
	if (!ret)
		ret = gc573_mmio_write32(&dev->mmio, GC573_VSCALER + 0x20, output_height);
	if (!ret)
		ret = gc573_mmio_write32(&dev->mmio, GC573_VSCALER + 0x28, vertical_step);
	if (!ret)
		ret = gc573_scaler_coefficients(dev, GC573_HSCALER);
	if (ret)
		return ret;

	/* The vendor generates phases through the larger of input/output width.
	 * Clear the unused tail of the 1024-entry RAM on every setup. */
	for (entry = 0; entry < GC573_SCALER_PHASE_ENTRIES; entry++) {
		u64 word = 0;
		u32 offset = GC573_HSCALER + 0x2000 + entry * 8;

		if (entry < (input_width > output_width ? input_width : output_width) / 4)
			word = gc573_scaler_build_phase_word(&acc, &lane,
				&output_pixel, output_width, horizontal_step);
		ret = gc573_mmio_write32(&dev->mmio, offset, lower_32_bits(word));
		if (!ret)
			ret = gc573_mmio_write32(&dev->mmio, offset + 4, upper_32_bits(word));
		if (ret)
			return ret;
	}

	ret = gc573_mmio_write32(&dev->mmio, GC573_HSCALER + 0x10, output_height);
	if (!ret)
		ret = gc573_mmio_write32(&dev->mmio, GC573_HSCALER + 0x18, input_width);
	if (!ret)
		ret = gc573_mmio_write32(&dev->mmio, GC573_HSCALER + 0x20, output_width);
	if (!ret)
		ret = gc573_mmio_write32(&dev->mmio, GC573_HSCALER + 0x28, 0);
	if (!ret)
		ret = gc573_mmio_write32(&dev->mmio, GC573_HSCALER + 0x30, horizontal_step);
	return ret;
}
