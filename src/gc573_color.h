/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef GC573_COLOR_H
#define GC573_COLOR_H

#include <linux/errno.h>
#include <linux/types.h>

/* AVI PB3 Q bits: 0=default, 1=limited, 2=full, 3=reserved. */
static inline int gc573_rgb_quantization_resolve(u8 q, u8 canonical_vic,
						 bool *limited)
{
	if (!limited || q > 3 || q == 3)
		return -EINVAL;

	switch (q) {
	case 1:
		*limited = true;
		return 0;
	case 2:
		*limited = false;
		return 0;
	default:
		break;
	}

	/* CTA VIC 1 uses full-range RGB by default. DMT VIC 0 is full-range;
	 * CTA 4K low-rate modes resolve through canonical VICs 93..95. */
	if (canonical_vic == 1 || canonical_vic == 0)
		*limited = false;
	else
		*limited = true;
	return 0;
}

#endif
