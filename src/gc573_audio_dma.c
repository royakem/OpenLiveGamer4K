// SPDX-License-Identifier: GPL-2.0-only
#include <linux/dma-mapping.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/workqueue.h>

#include "gc573_pure.h"
#include "gc573_audio_dma.h"

/* GC573 BAR0 audio DMA register map recovered from the vendor implementation. */
#define GC573_AUDIO_REG_CONTROL		0x008
#define GC573_AUDIO_REG_SELECTOR	0x014
#define GC573_AUDIO_REG_LENGTH0		0x200
#define GC573_AUDIO_REG_LENGTH1		0x204
#define GC573_AUDIO_REG_ADDR0_LO	0x208
#define GC573_AUDIO_REG_ADDR0_HI	0x20c
#define GC573_AUDIO_REG_ADDR1_LO	0x210
#define GC573_AUDIO_REG_ADDR1_HI	0x214
#define GC573_AUDIO_REG_INDEX		0x218
#define GC573_AUDIO_REG_LENGTH2		0x21c
#define GC573_AUDIO_REG_SEQUENCE0	0x2b4
#define GC573_AUDIO_REG_SEQUENCE1	0x2b8
#define GC573_AUDIO_REG_SEQUENCE2	0x2bc
#define GC573_AUDIO_REG_SEQUENCE3	0x2c0

#define GC573_AUDIO_IRQ_DONE		BIT(5)
#define GC573_AUDIO_CONTROL_ENABLE	BIT(1)
#define GC573_AUDIO_BLOCK_BYTES		3840U
#define GC573_AUDIO_DMA_BYTES		(11520U * 4U)
#define GC573_AUDIO_QUEUE_DEPTH		8U

static bool audio_dma_probe;
module_param(audio_dma_probe, bool, 0444);
MODULE_PARM_DESC(audio_dma_probe, "Diagnostic: mark fresh DMA buffers and discard untouched blocks");

struct gc573_audio_dma {
	spinlock_t lock;
	struct work_struct work;
	struct gc573_device *dev;
	gc573_audio_dma_push_t push;
	void *context;
	void *dma_cpu[2];
	dma_addr_t dma_addr[2];
	u8 *queue;
	u8 *scratch;
	unsigned int head;
	unsigned int tail;
	unsigned int queued;
	bool running;
	bool discontinuity;
	u64 irq_count;
	u64 bytes_queued;
	u64 bytes_delivered;
	u64 queue_overruns;
	u64 invalid_selector;
	u64 untouched_blocks;
	u64 nonzero_blocks;
	bool buffer_observed[2];
};

static void gc573_audio_dma_work(struct work_struct *work)
{
	struct gc573_audio_dma *audio =
		container_of(work, struct gc573_audio_dma, work);
	struct gc573_audio_dma_stats snapshot;
	unsigned long flags;

	for (;;) {
		spin_lock_irqsave(&audio->lock, flags);
		if (audio->discontinuity) {
			audio->discontinuity = false;
			audio->queued = 0;
			snapshot.running = audio->running;
			audio->running = false;
			spin_unlock_irqrestore(&audio->lock, flags);
			if (snapshot.running && audio->push)
				audio->push(audio->context, NULL, 0);
			return;
		}
		if (!audio->queued) {
			spin_unlock_irqrestore(&audio->lock, flags);
			return;
		}

		memcpy(audio->scratch,
		       audio->queue + audio->tail * GC573_AUDIO_BLOCK_BYTES,
		       GC573_AUDIO_BLOCK_BYTES);
		audio->tail = (audio->tail + 1) % GC573_AUDIO_QUEUE_DEPTH;
		audio->queued--;
		snapshot.running = audio->running;
		spin_unlock_irqrestore(&audio->lock, flags);

		/* The private scratch copy cannot be overwritten by the IRQ producer. */
		if (snapshot.running && audio->push) {
			audio->push(audio->context, audio->scratch,
				   GC573_AUDIO_BLOCK_BYTES);
			spin_lock_irqsave(&audio->lock, flags);
			audio->bytes_delivered += GC573_AUDIO_BLOCK_BYTES;
			spin_unlock_irqrestore(&audio->lock, flags);
		}
	}
}

int gc573_audio_dma_init(struct gc573_device *dev,
			 gc573_audio_dma_push_t push, void *context)
{
	struct gc573_audio_dma *audio;
	unsigned int i;

	if (!dev || !dev->pdev || !push || dev->audio_dma)
		return -EINVAL;

	audio = kzalloc(sizeof(*audio), GFP_KERNEL);
	if (!audio)
		return -ENOMEM;
	audio->dev = dev;
	audio->push = push;
	audio->context = context;
	spin_lock_init(&audio->lock);
	INIT_WORK(&audio->work, gc573_audio_dma_work);

	audio->queue = kmalloc_array(GC573_AUDIO_QUEUE_DEPTH,
				     GC573_AUDIO_BLOCK_BYTES, GFP_KERNEL);
	audio->scratch = kmalloc(GC573_AUDIO_BLOCK_BYTES, GFP_KERNEL);
	if (!audio->queue || !audio->scratch)
		goto err_free_host;

	for (i = 0; i < ARRAY_SIZE(audio->dma_cpu); i++) {
		audio->dma_cpu[i] = dma_alloc_coherent(&dev->pdev->dev,
						      GC573_AUDIO_DMA_BYTES,
						      &audio->dma_addr[i],
						      GFP_KERNEL);
		if (!audio->dma_cpu[i])
			goto err_free_dma;
		memset(audio->dma_cpu[i], audio_dma_probe ? 0xa5 : 0,
		       GC573_AUDIO_DMA_BYTES);
	}

	dev->audio_dma = audio;
	return 0;

err_free_dma:
	while (i--)
		dma_free_coherent(&dev->pdev->dev, GC573_AUDIO_DMA_BYTES,
				  audio->dma_cpu[i], audio->dma_addr[i]);
err_free_host:
	kfree(audio->scratch);
	kfree(audio->queue);
	kfree(audio);
	return -ENOMEM;
}

int gc573_audio_dma_start(struct gc573_device *dev)
{
	struct gc573_audio_dma *audio;
	unsigned long flags;
	unsigned int i;
	int ret = 0;

	if (!dev || !dev->audio_dma)
		return -ENODEV;
	audio = dev->audio_dma;

	spin_lock_irqsave(&audio->lock, flags);
	if (audio->running) {
		ret = -EBUSY;
		goto out_unlock;
	}

	/* Vendor setup uses a 3840-byte block and two coherent DMA buffers. */
	ret = gc573_mmio_write32(&dev->mmio, GC573_AUDIO_REG_LENGTH0, 0);
	if (ret)
		goto out_unlock;
	ret = gc573_mmio_write32(&dev->mmio, GC573_AUDIO_REG_SEQUENCE0, 0);
	if (ret)
		goto out_unlock;
	ret = gc573_mmio_write32(&dev->mmio, GC573_AUDIO_REG_SEQUENCE1, 1);
	if (ret)
		goto out_unlock;
	ret = gc573_mmio_write32(&dev->mmio, GC573_AUDIO_REG_SEQUENCE2, 2);
	if (ret)
		goto out_unlock;
	ret = gc573_mmio_write32(&dev->mmio, GC573_AUDIO_REG_SEQUENCE3, 3);
	if (ret)
		goto out_unlock;
	ret = gc573_mmio_write32(&dev->mmio, GC573_AUDIO_REG_LENGTH1,
				 GC573_AUDIO_BLOCK_BYTES / sizeof(u32));
	if (ret)
		goto out_unlock;
	ret = gc573_mmio_write32(&dev->mmio, GC573_AUDIO_REG_LENGTH2,
				 GC573_AUDIO_BLOCK_BYTES / sizeof(u32));
	if (ret)
		goto out_unlock;

	for (i = 0; i < ARRAY_SIZE(audio->dma_cpu); i++) {
		u32 lo = i ? GC573_AUDIO_REG_ADDR1_LO : GC573_AUDIO_REG_ADDR0_LO;
		u32 hi = i ? GC573_AUDIO_REG_ADDR1_HI : GC573_AUDIO_REG_ADDR0_HI;

		ret = gc573_mmio_write32(&dev->mmio, lo,
					 lower_32_bits(audio->dma_addr[i]));
		if (ret)
			goto out_unlock;
		ret = gc573_mmio_write32(&dev->mmio, hi,
					 upper_32_bits(audio->dma_addr[i]));
		if (ret)
			goto out_unlock;
	}
	ret = gc573_mmio_write32(&dev->mmio, GC573_AUDIO_REG_INDEX, 0);
	if (ret)
		goto out_unlock;

	dma_wmb();
	audio->head = 0;
	audio->tail = 0;
	audio->queued = 0;
	audio->discontinuity = false;
	audio->running = true;
	ret = gc573_mmio_maskwrite32(&dev->mmio, GC573_AUDIO_REG_CONTROL,
				     GC573_AUDIO_CONTROL_ENABLE,
				     GC573_AUDIO_CONTROL_ENABLE);
	if (ret)
		audio->running = false;

out_unlock:
	spin_unlock_irqrestore(&audio->lock, flags);
	return ret;
}

void gc573_audio_dma_stop(struct gc573_device *dev)
{
	struct gc573_audio_dma *audio;
	unsigned long flags;
	u32 control;

	if (!dev || !dev->audio_dma)
		return;
	audio = dev->audio_dma;

	spin_lock_irqsave(&audio->lock, flags);
	audio->running = false;
	/* Keep coherent buffers allocated; stop is allowed from ALSA trigger. */
	gc573_mmio_maskwrite32(&dev->mmio, GC573_AUDIO_REG_CONTROL,
			       GC573_AUDIO_CONTROL_ENABLE, 0);
	/* Flush the posted disable write before returning from the trigger path. */
	gc573_mmio_read32(&dev->mmio, GC573_AUDIO_REG_CONTROL, &control);
	spin_unlock_irqrestore(&audio->lock, flags);
}

void gc573_audio_dma_sync_stop(struct gc573_device *dev)
{
	struct gc573_audio_dma *audio;
	unsigned long flags;

	if (!dev || !dev->audio_dma)
		return;
	audio = dev->audio_dma;
	if (dev->hw.irq_requested)
		synchronize_irq(dev->hw.irq);
	cancel_work_sync(&audio->work);

	spin_lock_irqsave(&audio->lock, flags);
	audio->head = 0;
	audio->tail = 0;
	audio->queued = 0;
	spin_unlock_irqrestore(&audio->lock, flags);
}

void gc573_audio_dma_cleanup(struct gc573_device *dev)
{
	struct gc573_audio_dma *audio;
	unsigned int i;

	if (!dev || !dev->audio_dma)
		return;
	audio = dev->audio_dma;
	gc573_audio_dma_stop(dev);
	gc573_audio_dma_sync_stop(dev);

	/* Caller must have run pci_clear_master() before releasing DMA targets. */
	dev->audio_dma = NULL;
	for (i = 0; i < ARRAY_SIZE(audio->dma_cpu); i++)
		if (audio->dma_cpu[i])
			dma_free_coherent(&dev->pdev->dev, GC573_AUDIO_DMA_BYTES,
					  audio->dma_cpu[i], audio->dma_addr[i]);
	kfree(audio->scratch);
	kfree(audio->queue);
	kfree(audio);
}

void gc573_audio_dma_irq(struct gc573_device *dev, u32 status)
{
	struct gc573_audio_dma *audio;
	unsigned long flags;
	u32 selector;
	unsigned int buffer;

	if (!dev || !(status & GC573_AUDIO_IRQ_DONE) || !dev->audio_dma)
		return;
	audio = dev->audio_dma;

	spin_lock_irqsave(&audio->lock, flags);
	audio->irq_count++;
	spin_unlock_irqrestore(&audio->lock, flags);

	if (gc573_mmio_read32(&dev->mmio, GC573_AUDIO_REG_SELECTOR, &selector)) {
		spin_lock_irqsave(&audio->lock, flags);
		audio->invalid_selector++;
		spin_unlock_irqrestore(&audio->lock, flags);
		return;
	}
	selector &= 3;
	if (selector < 1 || selector > 2) {
		spin_lock_irqsave(&audio->lock, flags);
		audio->invalid_selector++;
		spin_unlock_irqrestore(&audio->lock, flags);
		return;
	}
	buffer = selector - 1;

	spin_lock_irqsave(&audio->lock, flags);
	if (!audio->running) {
		spin_unlock_irqrestore(&audio->lock, flags);
		return;
	}
	if (audio->queued == GC573_AUDIO_QUEUE_DEPTH) {
		audio->queue_overruns++;
		audio->discontinuity = true;
		spin_unlock_irqrestore(&audio->lock, flags);
		schedule_work(&audio->work);
		return;
	}
	/* Coherent DMA still needs ordering before CPU reads the completed block. */
	dma_rmb();
	if (audio_dma_probe && !audio->buffer_observed[buffer]) {
		if (!memchr_inv(audio->dma_cpu[buffer], 0xa5, GC573_AUDIO_BLOCK_BYTES)) {
			audio->untouched_blocks++;
			spin_unlock_irqrestore(&audio->lock, flags);
			return;
		}
		audio->buffer_observed[buffer] = true;
	}
	if (memchr_inv(audio->dma_cpu[buffer], 0, GC573_AUDIO_BLOCK_BYTES))
		audio->nonzero_blocks++;
	memcpy(audio->queue + audio->head * GC573_AUDIO_BLOCK_BYTES,
	       audio->dma_cpu[buffer], GC573_AUDIO_BLOCK_BYTES);
	audio->head = (audio->head + 1) % GC573_AUDIO_QUEUE_DEPTH;
	audio->queued++;
	audio->bytes_queued += GC573_AUDIO_BLOCK_BYTES;
	spin_unlock_irqrestore(&audio->lock, flags);

	/* Shared ISR owns the ACK; this hook only snapshots and queues the block. */
	queue_work(system_wq, &audio->work);
}

void gc573_audio_dma_get_stats(struct gc573_device *dev,
			       struct gc573_audio_dma_stats *stats)
{
	struct gc573_audio_dma *audio;
	unsigned long flags;

	if (!stats)
		return;
	memset(stats, 0, sizeof(*stats));
	if (!dev || !dev->audio_dma)
		return;
	audio = dev->audio_dma;

	spin_lock_irqsave(&audio->lock, flags);
	stats->running = audio->running;
	stats->irq_count = audio->irq_count;
	stats->bytes_queued = audio->bytes_queued;
	stats->bytes_delivered = audio->bytes_delivered;
	stats->queue_overruns = audio->queue_overruns;
	stats->invalid_selector = audio->invalid_selector;
	stats->untouched_blocks = audio->untouched_blocks;
	stats->nonzero_blocks = audio->nonzero_blocks;
	spin_unlock_irqrestore(&audio->lock, flags);
}
