/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef GC573_DESC_H
#define GC573_DESC_H

#include <linux/build_bug.h>
#include <linux/dma-mapping.h>
#include <linux/types.h>

#define GC573_DESC_MAX_ENTRIES 2048
#define GC573_DESC_HW_SLOTS 4
#define GC573_DESC_CONTROL 0x80006000U

/* Observed software-facing format; hardware interpretation is not proven. */
struct gc573_hw_desc {
	__le32 dma_addr_low;
	__le32 dma_addr_high;
	__le32 length_words;
	__le32 control;
} __packed;

static_assert(sizeof(struct gc573_hw_desc) == 16);

struct gc573_dma_segment {
	dma_addr_t dma_addr;
	size_t length_bytes;
};

struct gc573_desc_list {
	struct gc573_hw_desc *entries;
	unsigned int capacity;
	unsigned int count;
};

/* Software bookkeeping for the observed four-slot rotation only. */
struct gc573_desc_slots {
	u8 occupied;
	u8 next;
};

int gc573_desc_list_build(struct gc573_desc_list *list,
			  const struct gc573_dma_segment *segments,
			  unsigned int count);
int gc573_desc_list_build_range(struct gc573_desc_list *list,
				const struct gc573_dma_segment *segments,
				unsigned int count, size_t offset,
				size_t length);
int gc573_desc_slot_acquire(struct gc573_desc_slots *slots, u8 *slot);
int gc573_desc_slot_release(struct gc573_desc_slots *slots, u8 slot);

#endif /* GC573_DESC_H */
