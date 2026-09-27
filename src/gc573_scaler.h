/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef GC573_SCALER_H
#define GC573_SCALER_H
#include <linux/types.h>
struct gc573_device;
/* DMA must be stopped. Native progressive 4K input; supported outputs only. */
int gc573_scaler_setup(struct gc573_device *dev, u32 input_width,
		       u32 input_height, u32 output_width, u32 output_height);
#endif
