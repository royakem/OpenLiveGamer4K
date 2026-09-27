/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef GC573_HW_H
#define GC573_HW_H

#include <linux/dma-mapping.h>
#include <linux/bitops.h>
#include <linux/interrupt.h>
#include <linux/spinlock.h>
#include <linux/types.h>

#include "gc573_desc.h"

struct gc573_device;

#define GC573_REG_IRQ_STATUS		0x010
#define GC573_REG_DMA_CHANNEL		0x300
#define GC573_REG_DMA_DOORBELL		0x304
#define GC573_REG_DMA_DESC_BASE		0x308
#define GC573_REG_DMA_DESC_STRIDE	0x00c
#define GC573_REG_DMA_UV_DOORBELL	0x504
#define GC573_REG_DMA_UV_DESC_BASE	0x508
#define GC573_REG_DMA_DESC_COUNT	0x310
#define GC573_REG_STREAM_CONTROL	0x1000
#define GC573_REG_DMA_RESET		0x00c

#define GC573_IRQ_VIDEO_DONE		BIT(1)
#define GC573_IRQ_KNOWN_MASK		0x00000bffU

#define GC573_DESC_ALLOC_BYTES \
	(GC573_DESC_MAX_ENTRIES * sizeof(struct gc573_hw_desc))

struct gc573_hw_slot {
	struct gc573_hw_desc *descs[2];
	dma_addr_t dma[2];
	unsigned int count[2];
	void *cookie;
};

struct gc573_hw_engine {
	struct gc573_desc_slots slots;
	struct gc573_hw_slot slot[GC573_DESC_HW_SLOTS];
	spinlock_t lock;
	bool initialized;
	bool streaming;
	bool irq_vectors_allocated;
	bool irq_requested;
	int irq;
};

int gc573_hw_init(struct gc573_device *dev);
void gc573_hw_cleanup(struct gc573_device *dev);
int gc573_hw_request_irq(struct gc573_device *dev);
void gc573_hw_free_irq(struct gc573_device *dev);
int gc573_hw_submit(struct gc573_device *dev,
			const struct gc573_dma_segment *segments,
			unsigned int count, size_t plane0_bytes,
			 size_t plane1_bytes, void *cookie);
int gc573_hw_start(struct gc573_device *dev);
int gc573_hw_stop(struct gc573_device *dev);
int gc573_hw_reset_dma(struct gc573_device *dev);
irqreturn_t gc573_hw_irq(struct gc573_device *dev, void **cookie);

#endif /* GC573_HW_H */
