/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef GC573_PURE_H
#define GC573_PURE_H

#include <linux/pci.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include <linux/i2c.h>
#include <linux/completion.h>
#include <media/v4l2-device.h>
#include <media/v4l2-dev.h>

#include "gc573_desc.h"
#include "gc573_fpga.h"
#include "gc573_hw.h"

#define GC573_PCI_VENDOR_ID 0x1461
#define GC573_PCI_DEVICE_ID 0x0054
#define GC573_BAR 0

struct gc573_mmio {
	void __iomem *base;
	resource_size_t length;
};

/* Software bookkeeping only. This is not the card's hardware descriptor ABI. */
struct gc573_dma_buffer {
	dma_addr_t address;
	size_t length;
	void *owner;
};

struct gc573_dma_ring {
	struct gc573_dma_buffer *entries;
	unsigned int capacity;
	unsigned int head;
	unsigned int tail;
	unsigned int count;
	spinlock_t lock;
};

struct gc573_i2c_bus {
	struct i2c_adapter adapter;
	struct mutex lock;
	u8 channel;
	bool registered;
};

struct gc573_device;
struct gc573_audio_dma;
struct gc573_audio;
extern bool gc573_trace_frames;

struct gc573_i2c_ops {
	int (*read)(struct gc573_device *dev, u8 bus, u8 address,
		    u8 reg, u8 *value);
	int (*write)(struct gc573_device *dev, u8 bus, u8 address,
		     u8 reg, u8 value);
};

struct gc573_receiver_ops {
	int (*identify)(struct gc573_device *dev);
	int (*initialize)(struct gc573_device *dev);
	int (*get_signal_status)(struct gc573_device *dev, bool *locked);
	int (*configure_capture)(struct gc573_device *dev, u32 width,
				 u32 height, u32 fourcc);
};

struct gc573_irq_ops {
	/* Must read and acknowledge a documented device interrupt source. */
	int (*read_and_ack)(struct gc573_device *dev, u32 *pending);
};

struct gc573_device {
	struct pci_dev *pdev;
	struct gc573_mmio mmio;
	struct mutex lock;
	/* Source bridge initialization completed; enable stream-on recovery. */
	bool bridge_source;
	/* Validated live HDMI raster; process-side queue lock serializes updates. */
	u32 input_width;
	u32 input_height;
	u32 input_htotal;
	u32 input_vtotal;
	u32 input_vic;
	bool input_rgb_limited;
	struct gc573_i2c_bus i2c_bus;
	struct completion i2c_completion;
	spinlock_t i2c_state_lock;
	bool i2c_waiting;
	u8 i2c_completion_status;
	struct gc573_hw_engine hw;
	/*
	 * Measured OCLK base reference (kHz/2), stored at init so the
	 * experimental audio clock path can derive the audio TMDS clock.
	 * Zero until the receiver OCLK calibration has run.
	 */
	u32 oclk_reference_khz;
	struct gc573_audio_dma *audio_dma;
	struct gc573_audio *audio;
	/* Stream operations serialize through vdev->lock; DMA engines may coexist. */
	bool audio_running; /* vdev->lock */
	struct v4l2_device v4l2_dev;
	struct video_device *vdev;
	struct gc573_dma_ring dma_ring;
	const struct gc573_i2c_ops *i2c;
	const struct gc573_receiver_ops *it6805;
	const struct gc573_receiver_ops *it6664;
	const struct gc573_irq_ops *irq;
};

int gc573_mmio_read32(const struct gc573_mmio *mmio, u32 offset, u32 *value);
int gc573_mmio_write32(const struct gc573_mmio *mmio, u32 offset, u32 value);
int gc573_mmio_maskwrite32(const struct gc573_mmio *mmio, u32 offset,
				   u32 mask, u32 value);

int gc573_i2c_register(struct gc573_device *dev);
void gc573_i2c_unregister(struct gc573_device *dev);
int gc573_i2c_read_block(struct gc573_device *dev, u16 address, u8 reg,
			 u8 *buffer, u16 length);
int gc573_i2c_read_reg(struct gc573_device *dev, u16 address, u8 reg,
			   u8 *value);
int gc573_i2c_write_reg(struct gc573_device *dev, u16 address, u8 reg,
			    u8 value);
void gc573_i2c_irq(struct gc573_device *dev);

int gc573_dma_ring_init(struct gc573_dma_ring *ring, unsigned int capacity);
void gc573_dma_ring_cleanup(struct gc573_dma_ring *ring);
int gc573_dma_ring_push(struct gc573_dma_ring *ring,
			const struct gc573_dma_buffer *buffer);
int gc573_dma_ring_pop(struct gc573_dma_ring *ring,
		       struct gc573_dma_buffer *buffer);

int gc573_v4l2_register(struct gc573_device *dev);
void gc573_v4l2_unregister(struct gc573_device *dev);
void gc573_v4l2_buffer_done(struct gc573_device *dev, void *cookie);

extern const struct gc573_i2c_ops gc573_i2c_board_ops;
extern const struct gc573_receiver_ops gc573_it6805_source;
extern const struct gc573_receiver_ops gc573_it6664_unimplemented;
extern const struct gc573_irq_ops gc573_irq_unimplemented;

irqreturn_t gc573_irq_handler(int irq, void *opaque);

#endif /* GC573_PURE_H */
