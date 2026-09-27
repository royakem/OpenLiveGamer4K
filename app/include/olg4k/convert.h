/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef OLG4K_CONVERT_H
#define OLG4K_CONVERT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Convert one V4L2 frame buffer to packed RGB24 (little-endian plane).
 * Returns true on success; false if the format/geometry is invalid. */
bool olg4k_frame_to_rgb24(uint32_t fourcc, unsigned int width,
	unsigned int height, unsigned int bytesperline,
	const uint8_t *src, uint8_t *dst, size_t dst_len);
bool olg4k_frame_to_rgb24_extent(uint32_t fourcc, unsigned int width,
	unsigned int height, unsigned int bytesperline, size_t src_len,
	const uint8_t *src, uint8_t *dst, size_t dst_len,
	uint32_t ycbcr_enc, uint32_t quantization);

/* Swap red and blue in packed RGB24, in place; length must be divisible by 3. */
bool olg4k_rgb24_swap_red_blue(uint8_t *pixels, size_t length);

#endif
