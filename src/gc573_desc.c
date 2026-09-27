// SPDX-License-Identifier: GPL-2.0-only
#include <linux/types.h>
#include <linux/byteorder/little_endian.h>
#include <linux/errno.h>
#include <linux/overflow.h>
#include <linux/string.h>

#include "gc573_desc.h"

static int gc573_desc_segment_validate(const struct gc573_dma_segment *segment)
{
	u64 address = (u64)segment->dma_addr;
	u64 last_byte;

	if (!segment->length_bytes || (address & (sizeof(u32) - 1)) ||
	    (segment->length_bytes & (sizeof(u32) - 1)))
		return -EINVAL;

	if (segment->length_bytes / sizeof(u32) > U32_MAX)
		return -EOVERFLOW;

	if (check_add_overflow(address, (u64)segment->length_bytes - 1,
			       &last_byte))
		return -EOVERFLOW;

	return 0;
}

int gc573_desc_list_build(struct gc573_desc_list *list,
			  const struct gc573_dma_segment *segments,
			  unsigned int count)
{
	unsigned int i;
	int ret;

	if (!list || !list->entries || !list->capacity ||
	    (count && !segments) || count > list->capacity ||
	    count > GC573_DESC_MAX_ENTRIES)
		return -EINVAL;

	for (i = 0; i < count; i++) {
		ret = gc573_desc_segment_validate(&segments[i]);
		if (ret)
			return ret;
	}

	memset(list->entries, 0,
	       sizeof(*list->entries) * count);
	for (i = 0; i < count; i++) {
		u64 address = (u64)segments[i].dma_addr;

		list->entries[i].dma_addr_low = cpu_to_le32(lower_32_bits(address));
		list->entries[i].dma_addr_high = cpu_to_le32(upper_32_bits(address));
		list->entries[i].length_words =
			cpu_to_le32((u32)(segments[i].length_bytes / sizeof(u32)));
		list->entries[i].control = cpu_to_le32(GC573_DESC_CONTROL);
	}
	list->count = count;

	return 0;
}

/*
 * Build descriptors for a byte range in the concatenation of mapped SG
 * segments. Preflight the whole request before touching entries so failures
 * leave the output table unchanged and its count clear.
 */
int gc573_desc_list_build_range(struct gc573_desc_list *list,
				const struct gc573_dma_segment *segments,
				unsigned int count, size_t offset,
				size_t length)
{
	size_t total = 0, range_end, position = 0, remaining;
	unsigned int i, needed = 0, out = 0;

	if (!list)
		return -EINVAL;
	list->count = 0;
	if (!list->entries || !list->capacity || (count && !segments) ||
	    count > GC573_DESC_MAX_ENTRIES || !length ||
	    (offset & (sizeof(u32) - 1)) ||
	    (length & (sizeof(u32) - 1)))
		return -EINVAL;
	if (check_add_overflow(offset, length, &range_end))
		return -EOVERFLOW;

	for (i = 0; i < count; i++) {
		u64 address = (u64)segments[i].dma_addr;
		u64 last_byte;

		if (!segments[i].length_bytes ||
		    (address & (sizeof(u32) - 1)))
			return -EINVAL;
		if (check_add_overflow(address,
				       (u64)segments[i].length_bytes - 1,
				       &last_byte))
			return -EOVERFLOW;
		if (check_add_overflow(total, segments[i].length_bytes, &total))
			return -EOVERFLOW;
	}
	if (offset > total || length > total - offset)
		return -EINVAL;
	if (range_end > total)
		return -EINVAL;

	/* Validate each intersecting fragment and count required descriptors. */
	remaining = length;
	for (i = 0; i < count && remaining; i++) {
		size_t segment_end, skip, fragment;
		u64 address;

		if (check_add_overflow(position, segments[i].length_bytes,
				       &segment_end))
			return -EOVERFLOW;
		position = segment_end;
		if (position <= offset)
			continue;
		skip = offset > segment_end - segments[i].length_bytes ?
			offset - (segment_end - segments[i].length_bytes) : 0;
		fragment = segments[i].length_bytes - skip;
		if (fragment > remaining)
			fragment = remaining;
		address = (u64)segments[i].dma_addr + skip;
		if ((address & (sizeof(u32) - 1)) ||
		    (fragment & (sizeof(u32) - 1)))
			return -EINVAL;
		if (fragment / sizeof(u32) > U32_MAX)
			return -EOVERFLOW;
		if (++needed > list->capacity || needed > GC573_DESC_MAX_ENTRIES)
			return -ENOSPC;
		remaining -= fragment;
	}
	if (remaining)
		return -EINVAL;

	/* All validation is complete; now write the already-sized descriptor list. */
	position = 0;
	remaining = length;
	for (i = 0; i < count && remaining; i++) {
		size_t segment_start = position;
		size_t segment_end = position + segments[i].length_bytes;
		size_t skip, fragment;
		u64 address;

		position = segment_end;
		if (segment_end <= offset)
			continue;
		skip = offset > segment_start ? offset - segment_start : 0;
		fragment = segments[i].length_bytes - skip;
		if (fragment > remaining)
			fragment = remaining;
		address = (u64)segments[i].dma_addr + skip;
		list->entries[out].dma_addr_low = cpu_to_le32(lower_32_bits(address));
		list->entries[out].dma_addr_high = cpu_to_le32(upper_32_bits(address));
		list->entries[out].length_words = cpu_to_le32(fragment / sizeof(u32));
		list->entries[out].control = cpu_to_le32(GC573_DESC_CONTROL);
		out++;
		remaining -= fragment;
	}
	list->count = out;
	return 0;
}

int gc573_desc_slot_acquire(struct gc573_desc_slots *slots, u8 *slot)
{
	unsigned int i;

	if (!slots || !slot || slots->next >= GC573_DESC_HW_SLOTS ||
	    (slots->occupied & ~((1U << GC573_DESC_HW_SLOTS) - 1)))
		return -EINVAL;

	for (i = 0; i < GC573_DESC_HW_SLOTS; i++) {
		u8 candidate = (slots->next + i) % GC573_DESC_HW_SLOTS;
		u8 bit = 1U << candidate;

		if (slots->occupied & bit)
			continue;

		slots->occupied |= bit;
		slots->next = (candidate + 1) % GC573_DESC_HW_SLOTS;
		*slot = candidate;
		return 0;
	}

	return -ENOSPC;
}

int gc573_desc_slot_release(struct gc573_desc_slots *slots, u8 slot)
{
	u8 bit;

	if (!slots || slot >= GC573_DESC_HW_SLOTS ||
	    (slots->occupied & ~((1U << GC573_DESC_HW_SLOTS) - 1)))
		return -EINVAL;

	bit = 1U << slot;
	if (!(slots->occupied & bit))
		return -ENOENT;

	slots->occupied &= ~bit;
	return 0;
}
