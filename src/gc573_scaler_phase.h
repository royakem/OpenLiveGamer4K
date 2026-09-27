/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef GC573_SCALER_PHASE_H
#define GC573_SCALER_PHASE_H

#include <linux/types.h>

/*
 * Build one GC573 H-scaler phase RAM entry for its four pixels per clock.
 * The state values are carried between entries; output_width is in pixels
 * and step is the 16.16 input-pixels-per-output-pixel increment.
 */
static inline u64 gc573_scaler_build_phase_word(u32 *acc, u32 *lane,
						u32 *out, u32 output_width,
						u32 step)
{
	u64 packed = 0;
	unsigned int ppc_lane;

	for (ppc_lane = 0; ppc_lane < 4; ppc_lane++) {
		u32 phase = (*acc >> 10) & 0x3f;
		u32 valid = 0;

		if (*acc >> 16) {
			*acc -= 1U << 16;
			(*lane)++;
		}
		if (!(*acc >> 16) && *out < output_width) {
			*acc += step;
			(*out)++;
			valid = 1;
		}

		packed |= (u64)(phase | (*lane << 6) | (valid << 9)) <<
			  (ppc_lane * 10);
	}

	/* GC573's four-lane source index wraps at each packed phase word. */
	if (*lane >= 4)
		*lane &= 3;

	return packed;
}

#endif /* GC573_SCALER_PHASE_H */
