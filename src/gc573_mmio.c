// SPDX-License-Identifier: GPL-2.0-only
#include <linux/io.h>
#include <linux/overflow.h>

#include "gc573_pure.h"

static bool gc573_mmio_range_valid(const struct gc573_mmio *mmio, u32 offset)
{
	return mmio && mmio->base && IS_ALIGNED(offset, sizeof(u32)) &&
	       offset <= mmio->length &&
	       sizeof(u32) <= mmio->length - offset;
}

int gc573_mmio_read32(const struct gc573_mmio *mmio, u32 offset, u32 *value)
{
	if (!value || !gc573_mmio_range_valid(mmio, offset))
		return -EINVAL;

	*value = readl(mmio->base + offset);
	return 0;
}

int gc573_mmio_write32(const struct gc573_mmio *mmio, u32 offset, u32 value)
{
	if (!gc573_mmio_range_valid(mmio, offset))
		return -EINVAL;

	writel(value, mmio->base + offset);
	return 0;
}

int gc573_mmio_maskwrite32(const struct gc573_mmio *mmio, u32 offset,
				   u32 mask, u32 value)
{
	u32 old_value;
	int ret;

	ret = gc573_mmio_read32(mmio, offset, &old_value);
	if (ret)
		return ret;

	return gc573_mmio_write32(mmio, offset,
				  (old_value & ~mask) | (value & mask));
}
