/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef GC573_AUDIO_DMA_H
#define GC573_AUDIO_DMA_H

#include <linux/types.h>

struct gc573_device;

struct gc573_audio_dma_stats {
	bool running;
	u64 irq_count;
	u64 bytes_queued;
	u64 bytes_delivered;
	u64 queue_overruns;
	u64 invalid_selector;
	u64 untouched_blocks;
	u64 nonzero_blocks;
};

/* Worker callback: NULL/0 reports transport discontinuity (ALSA XRUN). */
typedef void (*gc573_audio_dma_push_t)(void *context, const u8 *data,
				       size_t length);

/*
 * Audio transport lifecycle. The embedding driver owns dev->audio_dma.
 * stop() is non-sleeping and only disables/marks the stream stopped;
 * sync_stop() is process-context and synchronizes IRQ/work after ALSA releases
 * its stream lock. cleanup() performs both phases and releases DMA memory;
 * the caller must stop and free/unregister the shared IRQ, then clear PCI bus
 * mastering before calling cleanup(). Queue overruns notify the frontend of an XRUN from deferred work.
 */
int gc573_audio_dma_init(struct gc573_device *dev,
			 gc573_audio_dma_push_t push, void *context);
int gc573_audio_dma_start(struct gc573_device *dev);
void gc573_audio_dma_stop(struct gc573_device *dev);
void gc573_audio_dma_sync_stop(struct gc573_device *dev);
void gc573_audio_dma_cleanup(struct gc573_device *dev);
void gc573_audio_dma_get_stats(struct gc573_device *dev,
			       struct gc573_audio_dma_stats *stats);

/* Called by the shared ISR with its status snapshot; this function never ACKs. */
void gc573_audio_dma_irq(struct gc573_device *dev, u32 status);

#endif /* GC573_AUDIO_DMA_H */
