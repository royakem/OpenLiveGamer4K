// SPDX-License-Identifier: GPL-2.0-only
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "gc573_pure.h"

int gc573_dma_ring_init(struct gc573_dma_ring *ring, unsigned int capacity)
{
	if (!ring || !capacity)
		return -EINVAL;

	memset(ring, 0, sizeof(*ring));
	ring->entries = kcalloc(capacity, sizeof(*ring->entries), GFP_KERNEL);
	if (!ring->entries)
		return -ENOMEM;

	ring->capacity = capacity;
	spin_lock_init(&ring->lock);
	return 0;
}

void gc573_dma_ring_cleanup(struct gc573_dma_ring *ring)
{
	if (!ring)
		return;

	kfree(ring->entries);
	memset(ring, 0, sizeof(*ring));
}

int gc573_dma_ring_push(struct gc573_dma_ring *ring,
			const struct gc573_dma_buffer *buffer)
{
	unsigned long flags;
	int ret = 0;

	if (!ring || !ring->entries || !buffer)
		return -EINVAL;

	spin_lock_irqsave(&ring->lock, flags);
	if (ring->count == ring->capacity) {
		ret = -ENOSPC;
	} else {
		ring->entries[ring->tail] = *buffer;
		ring->tail = (ring->tail + 1) % ring->capacity;
		ring->count++;
	}
	spin_unlock_irqrestore(&ring->lock, flags);
	return ret;
}

int gc573_dma_ring_pop(struct gc573_dma_ring *ring,
		       struct gc573_dma_buffer *buffer)
{
	unsigned long flags;
	int ret = 0;

	if (!ring || !ring->entries || !buffer)
		return -EINVAL;

	spin_lock_irqsave(&ring->lock, flags);
	if (!ring->count) {
		ret = -ENOENT;
	} else {
		*buffer = ring->entries[ring->head];
		ring->head = (ring->head + 1) % ring->capacity;
		ring->count--;
	}
	spin_unlock_irqrestore(&ring->lock, flags);
	return ret;
}
