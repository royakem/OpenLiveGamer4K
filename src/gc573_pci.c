// SPDX-License-Identifier: GPL-2.0-only
#include <linux/dma-mapping.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <media/videobuf2-core.h>

#include "gc573_pure.h"
#include "gc573_bridge_clock.h"
#include "gc573_bridge_rx.h"
#include "gc573_bridge_board.h"
#include "gc573_bridge_acquire.h"
#include "gc573_bridge_tx.h"
#include "gc573_audio_status.h"
#include "gc573_audio.h"
#include "gc573_audio_dma.h"

#define GC573_DRIVER_NAME "gc573_pure"

static bool audio_experimental;
module_param(audio_experimental, bool, 0444);
MODULE_PARM_DESC(audio_experimental,
	"Enable stereo 48 kHz HDMI PCM capture (legacy parameter name)");

static int gc573_pcm_start(void *context)
{
	struct gc573_device *dev = context;
	struct gc573_audio_status status;
	int ret;

	mutex_lock(dev->vdev->lock);
	ret = gc573_receiver_audio_snapshot(dev, &status);
	if (ret)
		goto out;
	if (!status.video_scdt || !status.receiver_reports_48k_lpcm_stereo) {
		ret = -ENODATA;
		goto out;
	}
	ret = gc573_receiver_prepare_audio(dev);
	if (!ret)
		ret = gc573_audio_dma_start(dev);
	if (!ret)
		dev->audio_running = true;
out:
	mutex_unlock(dev->vdev->lock);
	return ret;
}

static void gc573_pcm_stop(void *context)
{
	struct gc573_device *dev = context;

	mutex_lock(dev->vdev->lock);
	gc573_audio_dma_stop(dev);
	dev->audio_running = false;
	mutex_unlock(dev->vdev->lock);
}

static void gc573_pcm_sync_stop(void *context)
{
	gc573_audio_dma_sync_stop(context);
}

static void gc573_pcm_push(void *context, const u8 *data, size_t length)
{
	struct gc573_device *dev = context;

	if (!data)
		gc573_audio_xrun(dev->audio);
	else
		gc573_audio_push(dev->audio, data, length);
}

static const struct gc573_audio_ops gc573_pcm_ops = {
	.start = gc573_pcm_start,
	.stop = gc573_pcm_stop,
	.sync_stop = gc573_pcm_sync_stop,
};

/* Diagnostic snapshot, not an ALSA capture or audio-lock guarantee. The
 * V4L2 queue mutex also serializes receiver bank changes during link recovery.
 * Remove this file before tearing down the video device and its mutex. */
static ssize_t audio_status_show(struct device *device,
				struct device_attribute *attr, char *buf)
{
	struct gc573_device *dev = dev_get_drvdata(device);
	struct gc573_audio_status status;
	struct gc573_audio_dma_stats dma;
	u32 fpga_audio_detector;
	int ret;

	ret = mutex_lock_interruptible(dev->vdev->lock);
	if (ret)
		return ret;
	ret = gc573_receiver_audio_snapshot(dev, &status);
	gc573_audio_dma_get_stats(dev, &dma);
	if (!ret)
		ret = gc573_mmio_read32(&dev->mmio, 0x10a0, &fpga_audio_detector);
	mutex_unlock(dev->vdev->lock);
	if (ret)
		return ret;

	return sysfs_emit(buf,
		"experimental_pcm=%u\n"
		"infoframe_b0=%02x infoframe_b1=%02x infoframe_b2=%02x\n"
		"receiver_scdt_19=%02x audio_output_c7=%02x\n"
		"audio_control_81=%02x audio_control_8a=%02x audio_control_8c=%02x\n"
		"infoframe_valid=%u video_scdt=%u\n"
		"audio_rate_b5=%02x audio_rate_b6=%02x receiver_rate_code=%02x receiver_48k_lpcm_stereo=%u\n"
		"n_raw=%02x%02x%02x cts_raw=%02x%02x%02x n_decoded=%u cts_decoded=%u counters_coherent=%u\n"
		"dma_running=%u dma_irqs=%llu bytes_queued=%llu bytes_delivered=%llu queue_overruns=%llu invalid_selector=%llu untouched_blocks=%llu nonzero_blocks=%llu fpga_audio_detector=%08x\n",
		audio_experimental, status.infoframe_b0, status.infoframe_b1, status.infoframe_b2,
		status.receiver_scdt_19, status.audio_output_c7,
		status.audio_control_81, status.audio_control_8a, status.audio_control_8c,
		status.infoframe_valid, status.video_scdt,
		status.audio_rate_b5, status.audio_rate_b6, status.receiver_rate_code,
		status.receiver_reports_48k_lpcm_stereo,
		status.n_high, status.n_mid, status.n_low,
		status.cts_high, status.cts_mid, status.cts_low,
		status.n_decoded, status.cts_decoded, status.counters_coherent,
		dma.running, dma.irq_count, dma.bytes_queued, dma.bytes_delivered,
		dma.queue_overruns, dma.invalid_selector, dma.untouched_blocks,
		dma.nonzero_blocks, fpga_audio_detector);
}
static DEVICE_ATTR_RO(audio_status);

bool gc573_trace_frames;
module_param_named(trace_frames, gc573_trace_frames, bool, 0644);
MODULE_PARM_DESC(trace_frames, "Log every DMA queue/completion for bounded diagnostics (default off)");

static bool bridge_tx_only;
module_param(bridge_tx_only, bool, 0444);
MODULE_PARM_DESC(bridge_tx_only, "Diagnostic TX1 reconfiguration on an inherited RX link");

static bool bridge_link;
module_param(bridge_link, bool, 0444);
MODULE_PARM_DESC(bridge_link, "Experimental TX1 link setup; requires bridge_bootstrap");

static bool bridge_bootstrap;
module_param(bridge_bootstrap, bool, 0444);
MODULE_PARM_DESC(bridge_bootstrap, "Experimental switch clock and RX bootstrap; TX/runtime incomplete");

static void gc573_bridge_snapshot(struct gc573_device *dev, const char *phase)
{
	static const u8 maps[] = { 0x58, 0x70, 0x96, 0x68, 0x6a, 0x6c, 0x6e };
	u8 regs[32];
	unsigned int i;
	int ret;

	for (i = 0; i < ARRAY_SIZE(maps); i++) {
		ret = gc573_i2c_read_block(dev, maps[i], 0, regs, sizeof(regs));
		if (ret)
			dev_warn(&dev->pdev->dev, "bridge %s snapshot %02x error=%d\n",
				 phase, maps[i], ret);
		else
			dev_info(&dev->pdev->dev, "bridge %s snapshot %02x regs00..1f=%*ph\n",
				 phase, maps[i], (int)sizeof(regs), regs);
	}
	if (bridge_link || bridge_tx_only) {
		static const struct { u8 addr, reg; } ranges[] = {
			{0x58, 0x20}, {0x58, 0x40}, {0x58, 0x60},
			{0x58, 0x80}, {0x58, 0xa0}, {0x58, 0xc0},
			{0x70, 0x20}, {0x70, 0x40}, {0x70, 0xc0},
			{0x6a, 0x80}, {0x6a, 0xa0}, {0x6a, 0xc0},
			{0x90, 0x10}, {0x90, 0x90},
		};
		for (i = 0; i < ARRAY_SIZE(ranges); i++) {
			ret = gc573_i2c_read_block(dev, ranges[i].addr,
						  ranges[i].reg, regs, sizeof(regs));
			if (!ret)
				dev_info(&dev->pdev->dev,
					 "bridge %s extended %02x reg%02x=%*ph\n",
					 phase, ranges[i].addr, ranges[i].reg,
					 (int)sizeof(regs), regs);
		}
	}

}

static int gc573_probe(struct pci_dev *pdev,
		       const struct pci_device_id *id)
{
	struct gc573_device *dev;
	void __iomem * const *iomap_table;
	int ret;

	if (bridge_tx_only && (bridge_link || bridge_bootstrap))
		return -EINVAL;
	if (bridge_link && !bridge_bootstrap)
		return -EINVAL;

	dev = devm_kzalloc(&pdev->dev, sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;

	dev->pdev = pdev;
	dev->i2c = &gc573_i2c_board_ops;
	dev->it6805 = &gc573_it6805_source;
	dev->it6664 = &gc573_it6664_unimplemented;
	dev->irq = &gc573_irq_unimplemented;
	mutex_init(&dev->lock);
	pci_set_drvdata(pdev, dev);

	ret = pcim_enable_device(pdev);
	if (ret)
		return ret;

	if (!(pci_resource_flags(pdev, GC573_BAR) & IORESOURCE_MEM) ||
	    pci_resource_len(pdev, GC573_BAR) < sizeof(u32))
		return dev_err_probe(&pdev->dev, -ENODEV,
				     "BAR0 is not a usable memory resource\n");

	ret = pcim_iomap_regions(pdev, BIT(GC573_BAR), GC573_DRIVER_NAME);
	if (ret)
		return dev_err_probe(&pdev->dev, ret, "cannot map BAR0\n");

	iomap_table = pcim_iomap_table(pdev);
	if (!iomap_table || !iomap_table[GC573_BAR])
		return dev_err_probe(&pdev->dev, -ENOMEM, "BAR0 mapping missing\n");

	dev->mmio.base = iomap_table[GC573_BAR];
	dev->mmio.length = pci_resource_len(pdev, GC573_BAR);

	/* The working reference forces 32-bit DMA. */
	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "32-bit DMA mask unavailable\n");

	pci_set_master(pdev);
	ret = gc573_hw_init(dev);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "descriptor memory allocation failed\n");

	ret = gc573_fpga_init_registers(dev);
	if (ret)
		goto err_hw;

	ret = gc573_i2c_register(dev);
	if (ret)
		goto err_hw;
	msleep(100);
	gc573_bridge_snapshot(dev, "pre");

	if (bridge_bootstrap) {
		ret = gc573_bridge_clock_init(dev);
		if (ret) {
			dev_err(&pdev->dev, "bridge clock bootstrap failed: %d\n", ret);
			goto err_i2c;
		}
		dev_info(&pdev->dev, "bridge clock/maps bootstrap complete\n");
		ret = gc573_bridge_rx_init(dev);
		if (ret) {
			dev_err(&pdev->dev, "bridge RX bootstrap failed: %d\n", ret);
			goto err_i2c;
		}
		dev_info(&pdev->dev, "bridge RX bootstrap complete; TX/runtime pending\n");
		if (bridge_link) {
			ret = gc573_bridge_board_prepare(dev);
			if (ret)
				goto err_i2c;
			dev_info(&pdev->dev, "bridge shared switch/TX1 preparation complete\n");
		}
		gc573_bridge_snapshot(dev, "post-bootstrap");
	}

	ret = dev->it6805->identify(dev);
	if (ret)
		goto err_i2c;
	else
		dev_info(&pdev->dev,
			 "receiver pre-init identification accepted; IT6805 setup remains pending\n");

	ret = dev->it6805->initialize(dev);
	if (ret)
		goto err_i2c;

	ret = dev->it6805->identify(dev);
	if (ret)
		goto err_i2c;

	if (bridge_link) {
		ret = gc573_bridge_program_edid(dev);
		if (ret)
			goto err_i2c;
		ret = gc573_bridge_acquire_start(dev);
		if (!ret)
			ret = gc573_bridge_acquire_wait(dev);
		gc573_bridge_snapshot(dev, "post-acquisition");
		if (ret) {
			dev_err(&pdev->dev, "bridge RX acquisition failed: %d\n", ret);
			goto err_i2c;
		}
		ret = gc573_bridge_tx_enable_native(dev);
		gc573_bridge_snapshot(dev, "post-TX1-enable");
		if (ret) {
			dev_err(&pdev->dev, "bridge TX1 enable failed: %d\n", ret);
			goto err_i2c;
		}
		dev->bridge_source = true;
	}

	if (bridge_tx_only) {
		/* Diagnostic only: source TX reconfiguration, inherited RX acquisition. */
		msleep(2000);
		ret = gc573_bridge_tx_enable_native(dev);
		gc573_bridge_snapshot(dev, "TX1-only");
		if (ret)
			goto err_i2c;
	}

	ret = gc573_hw_request_irq(dev);
	if (ret)
		goto err_i2c;

	ret = gc573_v4l2_register(dev);
	if (ret)
		goto err_irq;
	ret = device_create_file(&pdev->dev, &dev_attr_audio_status);
	if (ret) {
		gc573_v4l2_unregister(dev);
		goto err_irq;
	}
	if (audio_experimental) {
		ret = gc573_audio_alloc(&pdev->dev, &gc573_pcm_ops, dev,
					&dev->audio);
		if (ret)
			goto err_audio;
		ret = gc573_audio_dma_init(dev, gc573_pcm_push, dev);
		if (ret)
			goto err_audio;
		ret = gc573_audio_register(dev->audio);
		if (ret)
			goto err_audio;
	}

	dev_info(&pdev->dev,
		 "bound in standalone source mode; capture path is under validation\n");
	return 0;

err_audio:
	device_remove_file(&pdev->dev, &dev_attr_audio_status);
	gc573_audio_cleanup(dev->audio);
	dev->audio = NULL;
	gc573_hw_free_irq(dev);
	pci_clear_master(pdev);
	gc573_audio_dma_cleanup(dev);
	gc573_v4l2_unregister(dev);
	goto err_i2c;

	err_i2c:
	gc573_i2c_unregister(dev);
	err_hw:
	gc573_hw_cleanup(dev);
	pci_clear_master(pdev);
	return dev_err_probe(&pdev->dev, ret, "standalone device setup failed\n");

err_irq:
	gc573_hw_free_irq(dev);
	goto err_i2c;
}

static void gc573_remove(struct pci_dev *pdev)
{
	struct gc573_device *dev = pci_get_drvdata(pdev);

	device_remove_file(&pdev->dev, &dev_attr_audio_status);
	gc573_audio_cleanup(dev->audio);
	dev->audio = NULL;
	gc573_v4l2_unregister(dev);
	gc573_i2c_unregister(dev);
	gc573_hw_free_irq(dev);
	gc573_hw_cleanup(dev);
	pci_clear_master(pdev);
	gc573_audio_dma_cleanup(dev);
	gc573_dma_ring_cleanup(&dev->dma_ring);
}

static const struct pci_device_id gc573_pci_ids[] = {
	{ PCI_DEVICE(GC573_PCI_VENDOR_ID, GC573_PCI_DEVICE_ID) },
	{ }
};
MODULE_DEVICE_TABLE(pci, gc573_pci_ids);

static struct pci_driver gc573_pci_driver = {
	.name = GC573_DRIVER_NAME,
	.id_table = gc573_pci_ids,
	.probe = gc573_probe,
	.remove = gc573_remove,
};
module_pci_driver(gc573_pci_driver);

MODULE_DESCRIPTION("OpenLiveGamer4K source driver for AVerMedia GC573");
MODULE_AUTHOR("Roy-Åke Martinsson <royakemartinsson@gmail.com>");
MODULE_LICENSE("GPL");
