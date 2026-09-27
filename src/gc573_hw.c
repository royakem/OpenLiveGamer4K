// SPDX-License-Identifier: GPL-2.0-only
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/pci.h>
#include <linux/slab.h>

#include "gc573_pure.h"
#include "gc573_audio_dma.h"
#include "gc573_audio.h"

static int gc573_hw_program_slot(struct gc573_device *dev, u8 slot)
{
	struct gc573_hw_slot *hw_slot = &dev->hw.slot[slot];
	unsigned int plane;
	u32 offset;
	int ret;

	/* Program both complete descriptor tables before ringing either channel.
	 * Packed formats use Y only; NV12's UV table starts at BAR0 0x508. */
	for (plane = 0; plane < 2; plane++) {
		offset = (plane ? GC573_REG_DMA_UV_DESC_BASE :
			GC573_REG_DMA_DESC_BASE) + GC573_REG_DMA_DESC_STRIDE * slot;
		ret = gc573_mmio_write32(&dev->mmio, offset,
			lower_32_bits(hw_slot->dma[plane]));
		if (!ret)
			ret = gc573_mmio_write32(&dev->mmio, offset + 4,
				upper_32_bits(hw_slot->dma[plane]));
		if (!ret)
			ret = gc573_mmio_write32(&dev->mmio, offset + 8,
				hw_slot->count[plane]);
		if (ret)
			return ret;
	}
	dma_wmb();
	/* UV is armed first so a running Y channel cannot see an unarmed plane. */
	if (hw_slot->count[1]) {
		ret = gc573_mmio_maskwrite32(&dev->mmio, GC573_REG_DMA_UV_DOORBELL,
			BIT(slot + 1), BIT(slot + 1));
		if (ret)
			return ret;
	}
	return gc573_mmio_maskwrite32(&dev->mmio, GC573_REG_DMA_DOORBELL,
			BIT(slot + 1), BIT(slot + 1));
}

int gc573_hw_reset_dma(struct gc573_device *dev)
{
	static const u32 reset_bits[] = { BIT(0), BIT(8), BIT(9) };
	unsigned int i;

	/* Process-side stream operations hold vdev->lock. The board-wide DMA
	 * reset is not qualified as audio-transparent: stop transport before
	 * resetting and report discontinuity asynchronously to ALSA. Never wait
	 * for the ALSA work here, since its stop callback takes vdev->lock. */
	if (dev->audio_running) {
		gc573_audio_dma_stop(dev);
		dev->audio_running = false;
		gc573_audio_xrun(dev->audio);
	}

	/* This is the sequence used by the reference reset(handle, 0x301):
	 * assert one reset bit at 0x0c, poll it clear, then pause before the next
	 * phase. The hardware reset meanings remain board-specific. */
	for (i = 0; i < ARRAY_SIZE(reset_bits); i++) {
		unsigned int tries;
		u32 value;
		int ret;

		ret = gc573_mmio_write32(&dev->mmio, GC573_REG_DMA_RESET,
					 reset_bits[i]);
		if (ret)
			return ret;

		for (tries = 0; tries < 10; tries++) {
			msleep(1);
			ret = gc573_mmio_read32(&dev->mmio,
						GC573_REG_DMA_RESET, &value);
			if (ret)
				return ret;
			if (!(value & reset_bits[i]))
				break;
		}
		msleep(30);
	}

	return 0;
}

int gc573_hw_init(struct gc573_device *dev)
{
	unsigned int i, plane;

	if (!dev || !dev->mmio.base)
		return -EINVAL;
	memset(&dev->hw, 0, sizeof(dev->hw));
	spin_lock_init(&dev->hw.lock);
	for (i = 0; i < GC573_DESC_HW_SLOTS; i++) {
		for (plane = 0; plane < 2; plane++) {
			dev->hw.slot[i].descs[plane] = dma_alloc_coherent(
				&dev->pdev->dev, GC573_DESC_ALLOC_BYTES,
				&dev->hw.slot[i].dma[plane], GFP_KERNEL);
			if (!dev->hw.slot[i].descs[plane]) {
				gc573_hw_cleanup(dev);
				return -ENOMEM;
			}
		}
	}
	dev->hw.initialized = true;
	return 0;
}

void gc573_hw_cleanup(struct gc573_device *dev)
{
	unsigned int i, plane;

	if (!dev)
		return;
	if (dev->hw.streaming)
		gc573_hw_stop(dev);
	for (i = 0; i < GC573_DESC_HW_SLOTS; i++) {
		for (plane = 0; plane < 2; plane++) {
			if (!dev->hw.slot[i].descs[plane])
				continue;
			dma_free_coherent(&dev->pdev->dev, GC573_DESC_ALLOC_BYTES,
				dev->hw.slot[i].descs[plane], dev->hw.slot[i].dma[plane]);
			dev->hw.slot[i].descs[plane] = NULL;
		}
	}
	dev->hw.initialized = false;
}

int gc573_hw_request_irq(struct gc573_device *dev)
{
	unsigned int flags = IRQF_SHARED;
	int ret;

	if (!dev || !dev->hw.initialized)
		return -EINVAL;

	ret = pci_alloc_irq_vectors(dev->pdev, 1, 1,
					PCI_IRQ_MSI | PCI_IRQ_INTX);
	if (ret < 0)
		return ret;
	dev->hw.irq_vectors_allocated = true;
	dev->hw.irq = pci_irq_vector(dev->pdev, 0);
	if (dev->pdev->msi_enabled)
		flags = 0;

	ret = request_irq(dev->hw.irq, gc573_irq_handler, flags,
			  "gc573_pure", dev);
	if (ret) {
		pci_free_irq_vectors(dev->pdev);
		dev->hw.irq_vectors_allocated = false;
		dev->hw.irq = 0;
		return ret;
	}

	dev->hw.irq_requested = true;
	return 0;
}

void gc573_hw_free_irq(struct gc573_device *dev)
{
	if (!dev)
		return;

	if (dev->hw.irq_requested) {
		free_irq(dev->hw.irq, dev);
		dev->hw.irq_requested = false;
	}
	if (dev->hw.irq_vectors_allocated) {
		pci_free_irq_vectors(dev->pdev);
		dev->hw.irq_vectors_allocated = false;
	}
	dev->hw.irq = 0;
}

int gc573_hw_submit(struct gc573_device *dev,
			const struct gc573_dma_segment *segments,
			unsigned int count, size_t plane0_bytes,
			 size_t plane1_bytes, void *cookie)
{
	struct gc573_desc_list list;
	unsigned long flags;
	u8 slot;
	int ret;

	if (!dev || !dev->hw.initialized || !segments || !count || !plane0_bytes || !cookie)
		return -EINVAL;

	spin_lock_irqsave(&dev->hw.lock, flags);
	ret = gc573_desc_slot_acquire(&dev->hw.slots, &slot);
	if (ret)
		goto unlock;

	list.entries = dev->hw.slot[slot].descs[0];
	list.capacity = GC573_DESC_MAX_ENTRIES;
	list.count = 0;
	ret = gc573_desc_list_build_range(&list, segments, count, 0, plane0_bytes);
	if (ret)
		goto release_slot;
	dev->hw.slot[slot].count[0] = list.count;
	dev->hw.slot[slot].count[1] = 0;
	if (plane1_bytes) {
		list.entries = dev->hw.slot[slot].descs[1];
		list.count = 0;
		ret = gc573_desc_list_build_range(&list, segments, count,
			plane0_bytes, plane1_bytes);
		if (ret)
			goto release_slot;
		dev->hw.slot[slot].count[1] = list.count;
	}
	dev->hw.slot[slot].cookie = cookie;
	ret = gc573_hw_program_slot(dev, slot);
	if (!ret && READ_ONCE(gc573_trace_frames))
		dev_info(&dev->pdev->dev,
			 "queued DMA slot %u descriptors=%u dma=%pad\n",
			 slot, dev->hw.slot[slot].count[0], &dev->hw.slot[slot].dma[0]);
	if (ret) {
		dev->hw.slot[slot].count[0] = 0;
		dev->hw.slot[slot].count[1] = 0;
		dev->hw.slot[slot].cookie = NULL;
		gc573_desc_slot_release(&dev->hw.slots, slot);
	}

unlock:
	spin_unlock_irqrestore(&dev->hw.lock, flags);
	return ret;

release_slot:
	dev->hw.slot[slot].count[0] = 0;
	dev->hw.slot[slot].count[1] = 0;
	gc573_desc_slot_release(&dev->hw.slots, slot);
	goto unlock;
}

int gc573_hw_start(struct gc573_device *dev)
{
	unsigned long flags;
	int ret = 0;

	if (!dev || !dev->hw.initialized)
		return -EINVAL;

	spin_lock_irqsave(&dev->hw.lock, flags);
	if (dev->hw.streaming)
		goto unlock;
	if (!dev->hw.slots.occupied) {
		ret = -ENODATA;
		goto unlock;
	}

	/* A format is common to all queued slots. Plane 1 exists only for NV12. */
	ret = gc573_mmio_maskwrite32(&dev->mmio, GC573_REG_DMA_UV_DOORBELL,
		BIT(0), dev->hw.slot[__ffs(dev->hw.slots.occupied)].count[1] ? BIT(0) : 0);
	if (ret)
		goto unlock;
	ret = gc573_mmio_maskwrite32(&dev->mmio, GC573_REG_DMA_DOORBELL,
					  BIT(0), BIT(0));
	if (ret)
		goto unlock;
	ret = gc573_mmio_maskwrite32(&dev->mmio, GC573_REG_STREAM_CONTROL,
					  BIT(0), BIT(0));
	if (!ret) {
		dev->hw.streaming = true;
		dev_info(&dev->pdev->dev, "DMA stream started\n");
	}

unlock:
	spin_unlock_irqrestore(&dev->hw.lock, flags);
	return ret;
}

int gc573_hw_stop(struct gc573_device *dev)
{
	unsigned long flags;
	unsigned int tries;
	u32 value;
	bool was_streaming;
	bool had_descriptors;
	int ret = 0;

	if (!dev || !dev->hw.initialized)
		return -EINVAL;

	spin_lock_irqsave(&dev->hw.lock, flags);
	was_streaming = dev->hw.streaming;
	had_descriptors = dev->hw.slots.occupied != 0;
	if (was_streaming || had_descriptors) {
		ret = gc573_mmio_maskwrite32(&dev->mmio,
					     GC573_REG_STREAM_CONTROL, BIT(0), 0);
		dev->hw.streaming = false;
	}
	spin_unlock_irqrestore(&dev->hw.lock, flags);
	if (dev->hw.irq_requested)
		synchronize_irq(dev->hw.irq);

	/* The reference waits for its completion DPC. The live card exposes the
	 * same active bit in the read-only stream samples, so use it as a bounded
	 * drain indication while keeping teardown finite. */
	if ((was_streaming || had_descriptors) && !ret) {
		for (tries = 0; tries < 200; tries++) {
			msleep(10);
			if (gc573_mmio_read32(&dev->mmio,
					      GC573_REG_STREAM_CONTROL, &value))
				break;
			if (!(value & BIT(0)))
				break;
		}
		ret = gc573_hw_reset_dma(dev);
	}
	dev_info(&dev->pdev->dev, "DMA stream stopped ret=%d\n", ret);

	spin_lock_irqsave(&dev->hw.lock, flags);
	for (tries = 0; tries < GC573_DESC_HW_SLOTS; tries++) {
		dev->hw.slot[tries].count[0] = 0;
		dev->hw.slot[tries].count[1] = 0;
		dev->hw.slot[tries].cookie = NULL;
	}
	dev->hw.slots.occupied = 0;
	dev->hw.slots.next = 0;
	spin_unlock_irqrestore(&dev->hw.lock, flags);
	return ret;
}

irqreturn_t gc573_hw_irq(struct gc573_device *dev, void **cookie)
{
	u32 status;
	u32 channel;
	u32 mask;
	u8 slot;
	unsigned long flags;
	int ret;

	if (!dev || !dev->hw.initialized || !cookie)
		return IRQ_NONE;
	*cookie = NULL;

	ret = gc573_mmio_read32(&dev->mmio, GC573_REG_IRQ_STATUS, &status);
	if (ret || !(status & GC573_IRQ_KNOWN_MASK))
		return IRQ_NONE;
	dev_dbg(&dev->pdev->dev, "IRQ status=0x%08x\n", status);

	if (status & BIT(11))
		gc573_i2c_irq(dev);
	if (status & BIT(5))
		gc573_audio_dma_irq(dev, status);

	if (status & GC573_IRQ_VIDEO_DONE) {
		ret = gc573_mmio_read32(&dev->mmio, GC573_REG_DMA_CHANNEL,
					&channel);
		if (!ret && (channel & 0x7) >= 1 && (channel & 0x7) <= 4) {
			slot = (channel & 0x7) - 1;
			mask = BIT(slot + 1);

			spin_lock_irqsave(&dev->hw.lock, flags);
			if (dev->hw.slots.occupied & BIT(slot)) {
				*cookie = dev->hw.slot[slot].cookie;
				dev->hw.slot[slot].cookie = NULL;
				dev->hw.slot[slot].count[0] = 0;
		dev->hw.slot[slot].count[1] = 0;
				gc573_desc_slot_release(&dev->hw.slots, slot);
			}
			spin_unlock_irqrestore(&dev->hw.lock, flags);

			/* The reference clears the completed slot only if its bit is set. */
			ret = gc573_mmio_read32(&dev->mmio, GC573_REG_DMA_DOORBELL,
						&channel);
			if (!ret && (channel & mask))
				gc573_mmio_write32(&dev->mmio,
						   GC573_REG_DMA_DOORBELL,
						   channel & ~mask);
		}
	}

	/* Acknowledge the status bits with the same read-modify-write operation
	 * used by the reference. Bit 11 uses a literal write in that path. */
	mask = status & (GC573_IRQ_KNOWN_MASK & ~BIT(11));
	if (mask)
		gc573_mmio_maskwrite32(&dev->mmio, GC573_REG_IRQ_STATUS,
					mask, mask);
	if (status & BIT(11))
		gc573_mmio_write32(&dev->mmio, GC573_REG_IRQ_STATUS, BIT(11));

	return IRQ_HANDLED;
}
