/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef GC573_INPUT_MODES_H
#define GC573_INPUT_MODES_H

#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/videodev2.h>
#include <linux/v4l2-dv-timings.h>

struct gc573_input_mode {
	struct v4l2_dv_timings timings;
	u8 vic;
};

/* Exact CTA raster timings, plus the two established 1280x800 DMT modes. */
static const struct gc573_input_mode gc573_input_modes[] = {
	{ V4L2_DV_BT_CEA_1280X720P24, 60 },
	{ V4L2_DV_BT_CEA_1280X720P25, 61 },
	{ V4L2_DV_BT_CEA_1280X720P30, 62 },
	{ V4L2_DV_BT_CEA_1280X720P50, 19 },
	{ V4L2_DV_BT_CEA_1280X720P60, 4 },
	{ V4L2_DV_BT_CEA_1920X1080P24, 32 },
	{ V4L2_DV_BT_CEA_1920X1080P25, 33 },
	{ V4L2_DV_BT_CEA_1920X1080P30, 34 },
	{ V4L2_DV_BT_CEA_1920X1080P50, 31 },
	{ V4L2_DV_BT_CEA_1920X1080P60, 16 },
	{ V4L2_DV_BT_CEA_3840X2160P24, 93 },
	{ V4L2_DV_BT_CEA_3840X2160P25, 94 },
	{ V4L2_DV_BT_CEA_3840X2160P30, 95 },
	{ V4L2_DV_BT_CEA_3840X2160P50, 96 },
	{ V4L2_DV_BT_CEA_3840X2160P60, 97 },
	{ V4L2_DV_BT_DMT_1280X800P60, 0 },
	{ V4L2_DV_BT_DMT_1280X800P60_RB, 0 },
};

static inline const struct gc573_input_mode *
gc573_input_mode_find(u8 vic, u32 width, u32 height, u32 htotal, u32 vtotal)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(gc573_input_modes); i++) {
		const struct v4l2_bt_timings *bt =
			&gc573_input_modes[i].timings.bt;
		u32 mode_htotal = bt->width + bt->hfrontporch + bt->hsync +
			bt->hbackporch;
		u32 mode_vtotal = bt->height + bt->vfrontporch + bt->vsync +
			bt->vbackporch;

		if (bt->interlaced || bt->width != width || bt->height != height ||
		    mode_htotal != htotal || mode_vtotal != vtotal)
			continue;
		if (vic) {
			if (gc573_input_modes[i].vic != vic)
				continue;
		} else if (gc573_input_modes[i].vic &&
			   (gc573_input_modes[i].vic < 93 ||
			    gc573_input_modes[i].vic > 95)) {
			continue;
		}
		return &gc573_input_modes[i];
	}
	return NULL;
}

static inline bool gc573_input_mode_clock_valid(
		const struct gc573_input_mode *mode, u32 measured_khz)
{
	u32 nominal_khz;

	if (!mode || !measured_khz)
		return false;
	nominal_khz = mode->timings.bt.pixelclock / 1000;
	return measured_khz >= nominal_khz * 3 / 4 &&
		measured_khz <= nominal_khz * 11 / 10;
}

#endif /* GC573_INPUT_MODES_H */
