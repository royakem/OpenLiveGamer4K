/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef GC573_FORMATS_H
#define GC573_FORMATS_H

#include <linux/overflow.h>
#include <linux/errno.h>
#include <linux/types.h>
#include <linux/videodev2.h>

static inline bool gc573_format_supported(u32 fourcc)
{
	return fourcc == V4L2_PIX_FMT_YUYV || fourcc == V4L2_PIX_FMT_NV12 ||
	       fourcc == V4L2_PIX_FMT_RGB24 || fourcc == V4L2_PIX_FMT_BGR24;
}

/* Only progressive SDR 8-bit formats are supported. */
static inline int gc573_format_layout(u32 fourcc, u32 width, u32 height,
				      u32 *bytesperline, u32 *sizeimage)
{
	u32 stride, size;

	if (!bytesperline || !sizeimage || !width || !height ||
	    (width & 1) || (height & 1) || !gc573_format_supported(fourcc))
		return -EINVAL;

	switch (fourcc) {
	case V4L2_PIX_FMT_YUYV:
		if (check_mul_overflow(width, 2U, &stride) ||
		    check_mul_overflow(stride, height, &size))
			return -EOVERFLOW;
		break;
	case V4L2_PIX_FMT_NV12:
		if (check_mul_overflow(width, height, &size))
			return -EOVERFLOW;
		if (size > U32_MAX - size / 2)
			return -EOVERFLOW;
		size += size / 2;
		stride = width;
		break;
	case V4L2_PIX_FMT_RGB24:
	case V4L2_PIX_FMT_BGR24:
		if (check_mul_overflow(width, 3U, &stride) ||
		    check_mul_overflow(stride, height, &size))
			return -EOVERFLOW;
		break;
	default:
		return -EINVAL;
	}
	*bytesperline = stride;
	*sizeimage = size;
	return 0;
}

/* Convert the BGR24 bytes supplied by hardware to userspace RGB24 in place. */
static inline int gc573_rgb24_swap_red_blue(u8 *pixels, size_t length)
{
	size_t i;

	if (!pixels || length % 3)
		return -EINVAL;
	for (i = 0; i < length; i += 3) {
		u8 red = pixels[i];

		pixels[i] = pixels[i + 2];
		pixels[i + 2] = red;
	}
	return 0;
}

#endif
