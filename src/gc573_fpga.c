// SPDX-License-Identifier: GPL-2.0-only
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/videodev2.h>

#include "gc573_fpga.h"
#include "gc573_scaler.h"
#include "gc573_pure.h"

#define GC573_REG_FPGA_RECONFIG	0x107c
#define GC573_REG_FPGA_VERSION	0x0000
#define GC573_REG_IRQ_ENABLE	0x001c
#define GC573_REG_GPIO		0x0040
#define GC573_REG_CAPTURE_MODE	0x50000
#define GC573_REG_VIP_CONTROL	0x1000
#define GC573_REG_VIP_FRAME_RATE	0x1080
#define GC573_REG_VIP_FRAME_PERIOD	0x103c
#define GC573_REG_VIP_PIXEL_MODE	0x1088
#define GC573_REG_VIP_INPUT_X	0x1020
#define GC573_REG_VIP_INPUT_WIDTH	0x1024
#define GC573_REG_VIP_INPUT_Y	0x1028
#define GC573_REG_VIP_INPUT_HEIGHT	0x102c

#define GC573_VIP_CONTROL_CFG_MASK	(BIT(0) | BIT(2) | BIT(3) | BIT(6) | \
						 BIT(7) | GENMASK(15, 8))
#define GC573_VIP_NATIVE_CLOCK_HZ	148500000U
#define GC573_VIP_NATIVE_WIDTH		3840U
#define GC573_VIP_NATIVE_HEIGHT		2160U
#define GC573_VIP_NATIVE_FPS		60U

#define GC573_IRQ_ENABLE_MASK	0x00000bffU

static bool preserve_bridge;
module_param(preserve_bridge, bool, 0444);
MODULE_PARM_DESC(preserve_bridge,
		 "Skip board GPIO reset sequence for warm-handoff diagnosis (default: false)");

static unsigned int reset_gpio_mask = BIT(5) | BIT(8);
module_param(reset_gpio_mask, uint, 0444);
MODULE_PARM_DESC(reset_gpio_mask,
		 "Diagnostic GPIO reset mask: bit5/bit8, default both (288)");

static int gc573_fpga_set_gpio(struct gc573_device *dev, unsigned int pin,
				       bool high)
{
	u32 value;
	int ret;

	if (pin >= 32)
		return -EINVAL;

	ret = gc573_mmio_read32(&dev->mmio, GC573_REG_GPIO, &value);
	if (ret)
		return ret;
	if (high)
		value |= BIT(pin);
	else
		value &= ~BIT(pin);
	return gc573_mmio_write32(&dev->mmio, GC573_REG_GPIO, value);
}

static int gc573_fpga_set_gpio_until_high(struct gc573_device *dev,
						  unsigned int pin)
{
	unsigned int tries;
	u32 value;
	int ret;

	for (tries = 0; tries < 20; tries++) {
		ret = gc573_fpga_set_gpio(dev, pin, true);
		if (ret)
			return ret;
		msleep(5);
		ret = gc573_mmio_read32(&dev->mmio, GC573_REG_GPIO, &value);
		if (ret)
			return ret;
		if (value & BIT(pin))
			return 0;
	}
	return -ETIMEDOUT;
}

static int gc573_fpga_reconfigure(struct gc573_device *dev)
{
	u32 value;
	unsigned int attempt;
	int ret;

	ret = gc573_mmio_read32(&dev->mmio, GC573_REG_FPGA_RECONFIG, &value);
	if (ret)
		return ret;
	if (value & BIT(0))
		return 0;

	for (attempt = 0; attempt < 3; attempt++) {
		ret = gc573_mmio_write32(&dev->mmio, GC573_REG_FPGA_RECONFIG, 0x8);
		if (ret)
			return ret;
		ret = gc573_mmio_write32(&dev->mmio, GC573_REG_FPGA_RECONFIG, 0x1);
		if (ret)
			return ret;
		msleep(2000);
		ret = gc573_mmio_read32(&dev->mmio, GC573_REG_FPGA_RECONFIG,
					&value);
		if (ret)
			return ret;
		if (value & BIT(0))
			return 0;
	}

	dev_warn(&dev->pdev->dev,
		 "FPGA reconfiguration ready bit did not assert after three attempts\n");
	return 0;
}

int gc573_fpga_init_registers(struct gc573_device *dev)
{
	u32 value;
	int ret;

	if (!dev)
		return -EINVAL;

	ret = gc573_hw_reset_dma(dev);
	if (ret)
		return ret;

	ret = gc573_fpga_reconfigure(dev);
	if (ret)
		return ret;

	ret = gc573_mmio_read32(&dev->mmio, GC573_REG_IRQ_ENABLE, &value);
	if (ret)
		return ret;
	ret = gc573_mmio_write32(&dev->mmio, GC573_REG_IRQ_ENABLE,
				 value & GC573_IRQ_ENABLE_MASK);
	if (ret)
		return ret;
	ret = gc573_mmio_write32(&dev->mmio, GC573_REG_IRQ_ENABLE, 0x00000b33);
	if (ret)
		return ret;
	ret = gc573_mmio_write32(&dev->mmio, GC573_REG_CAPTURE_MODE, 0x3);
	if (ret)
		return ret;

	if (preserve_bridge) {
		dev_warn(&dev->pdev->dev,
			 "preserve_bridge=1: warm-handoff diagnostic; skipping board GPIO reset sequence; this is not cold standalone initialization\n");
		goto read_version;
	}

	/* These GPIO transitions are the observed board reset/settle sequence.
	 * Their electrical names are intentionally left out until the board
	 * routing is independently identified. */
	ret = gc573_fpga_set_gpio_until_high(dev, 4);
	if (ret)
		dev_dbg(&dev->pdev->dev, "GPIO4 did not report high during settle\n");

	ret = gc573_fpga_set_gpio_until_high(dev, 6);
	if (ret)
		dev_dbg(&dev->pdev->dev, "GPIO6 did not report high during settle\n");

	msleep(2);
	ret = gc573_fpga_set_gpio(dev, 1, false);
	if (ret)
		return ret;
	msleep(2);
	if (reset_gpio_mask & BIT(5)) {
		ret = gc573_fpga_set_gpio(dev, 5, true);
		if (ret)
			return ret;
		msleep(5);
		ret = gc573_fpga_set_gpio(dev, 5, false);
		if (ret)
			return ret;
		msleep(10);
		ret = gc573_fpga_set_gpio(dev, 5, true);
		if (ret)
			return ret;
		msleep(5);
	}
	if (reset_gpio_mask & BIT(8)) {
		ret = gc573_fpga_set_gpio(dev, 8, true);
		if (ret)
			return ret;
		msleep(2);
		ret = gc573_fpga_set_gpio(dev, 8, false);
		if (ret)
			return ret;
		msleep(2);
		ret = gc573_fpga_set_gpio(dev, 8, true);
		if (ret)
			return ret;
	}

read_version:
	ret = gc573_mmio_read32(&dev->mmio, GC573_REG_FPGA_VERSION, &value);
	if (!ret)
		dev_info(&dev->pdev->dev, "FPGA version 0x%08x\n", value);
	return ret;
}

int gc573_fpga_configure_capture(struct gc573_device *dev,
					 u32 input_width, u32 input_height,
					 u32 width, u32 height, u32 rate_num, u32 rate_den,
					 u32 pixelformat)
{
	/* BT.709 full-range RGB -> limited YUV matrix. These packed values
	 * match the working GC573 BAR0 sample and the GPL GC555 source matrix.
	 * Explicit programming removes the warm-handoff coefficient dependency. */
	static const u32 rgb_full_to_yuv[] = {
		0x00000350, 0x03f8274f, 0x01000bb0, 0x1c1c15ac,
		0x08000670, 0x02941988, 0x08001c1c,
	};
	static const u32 rgb_limited_to_yuv[] = {
		0x00000350, 0x049f2dc6, 0x00000d9b, 0x20bb193b,
		0x08000780, 0x03001dba, 0x080020bb,
	};
	static const u32 rgb_limited_to_full[] = {
		0x00000888, 0x00004a85, 0x012a0000, 0x4a850000,
		0x012a0000, 0x00000000, 0x012a4a85,
	};
	const u32 *coefficients;
	unsigned int index;
	u32 value, fps, period, format_selector, color_control;
	bool rgb_output;
	int ret;

	if (!dev || !width || !height || !rate_num || !rate_den)
		return -EINVAL;

	if (!((rate_den == 1 && (rate_num == 24 || rate_num == 25 ||
		 rate_num == 30 || rate_num == 50 || rate_num == 60)) ||
	      (rate_den == 1001 && (rate_num == 24000 || rate_num == 30000 ||
		 rate_num == 60000))))
		return -EINVAL;
	fps = DIV_ROUND_CLOSEST(rate_num, rate_den);
	period = div_u64((u64)GC573_VIP_NATIVE_CLOCK_HZ * rate_den, rate_num);

	/* GC573 reference format selector: NV12=15, packed BGR=2. RGB24
	 * uses the same hardware packing, then swaps R/B at VB2 dequeue. */
	rgb_output = pixelformat == V4L2_PIX_FMT_RGB24 ||
		pixelformat == V4L2_PIX_FMT_BGR24;
	switch (pixelformat) {
	case V4L2_PIX_FMT_YUYV:
		format_selector = 0;
		break;
	case V4L2_PIX_FMT_NV12:
		format_selector = 0x0f << 8;
		break;
	case V4L2_PIX_FMT_RGB24:
	case V4L2_PIX_FMT_BGR24:
		format_selector = 0x02 << 8;
		break;
	default:
		return -EINVAL;
	}

	/* Input geometry was validated against standard timings upstream. */
	if (!((input_width == 3840 && input_height == 2160) ||
	      (input_width == 1920 && input_height == 1080) ||
	      (input_width == 1280 && (input_height == 720 || input_height == 800))) ||
	    !((width == 3840 && height == 2160) ||
	      (width == 1920 && height == 1080) ||
	      (width == 1280 && (height == 720 || height == 800))))
		return -EINVAL;

	/* Use the equal-dimension bypass branch from the reference helper.
	 * Input packing and RGB-to-YUV conversion are configured separately. */
	ret = gc573_mmio_maskwrite32(&dev->mmio, GC573_REG_VIP_CONTROL,
					     GC573_VIP_CONTROL_CFG_MASK,
					     format_selector);
	if (ret)
		return ret;

	/* All supported frame spans are 32-byte aligned in every format.
	 * Reference TLP fix mode 0, preserving unrelated BAR control bits. */
	ret = gc573_mmio_maskwrite32(&dev->mmio, 0x0004, GENMASK(1, 0), 0);
	if (ret)
		return ret;

	/* Reset the VIP after the receiver changes its output mode, before
	 * reprogramming dimensions and color coefficients (reference order). */
	ret = gc573_mmio_write32(&dev->mmio, GC573_REG_CAPTURE_MODE, 0);
	if (ret)
		return ret;
	msleep(20);
	ret = gc573_mmio_write32(&dev->mmio, GC573_REG_CAPTURE_MODE, 3);
	if (ret)
		return ret;

	ret = gc573_mmio_write32(&dev->mmio, GC573_REG_VIP_INPUT_X, 0);
	if (ret)
		return ret;
	ret = gc573_mmio_write32(&dev->mmio, GC573_REG_VIP_INPUT_WIDTH,
					 input_width);
	if (ret)
		return ret;
	ret = gc573_mmio_write32(&dev->mmio, GC573_REG_VIP_INPUT_Y, 0);
	if (ret)
		return ret;
	ret = gc573_mmio_write32(&dev->mmio, GC573_REG_VIP_INPUT_HEIGHT,
					 input_height);
	if (ret)
		return ret;

	ret = gc573_mmio_read32(&dev->mmio, GC573_REG_VIP_FRAME_RATE, &value);
	if (ret)
		return ret;
	/* Current input is RGB8; bit1 is the input-YUV flag, bit2 RGB output. */
	value = (value & (0xffffU & ~(BIT(1) | BIT(2)))) | (fps << 16);
	if (rgb_output)
		value |= BIT(2);
	ret = gc573_mmio_write32(&dev->mmio, GC573_REG_VIP_FRAME_RATE, value);
	if (ret)
		return ret;

	ret = gc573_mmio_write32(&dev->mmio, GC573_REG_VIP_FRAME_PERIOD,
					 period);
	if (ret)
		return ret;
	/* Native 4K receiver output is dual-pixel DDR. The vendor configuration
	 * combines those flags as 3 at 0x1088 (not a YUYV format selector).
	 * See maps/native-pipeline-20260924.md and the working register sample. */
	ret = gc573_mmio_write32(&dev->mmio, GC573_REG_VIP_PIXEL_MODE,
					 input_width == 3840 ? 3 : 0);
	if (ret)
		return ret;
	/* RGB packet range is resolved from the checked AVI frame. Select the
	 * matrix before loading its coefficients, following the reference API.
	 * Full RGB output bypasses conversion; limited RGB is expanded once. */
	color_control = input_width == 3840 ? BIT(5) : 0;
	if (rgb_output) {
		coefficients = rgb_limited_to_full;
		if (dev->input_rgb_limited)
			color_control |= (6U << 8) | BIT(1);
	} else {
		coefficients = dev->input_rgb_limited ?
			rgb_limited_to_yuv : rgb_full_to_yuv;
		color_control |= BIT(1);
		if (dev->input_rgb_limited)
			color_control |= 1U << 8;
	}
	ret = gc573_mmio_write32(&dev->mmio, 0x1040, color_control);
	if (ret)
		return ret;
	for (index = 0; index < ARRAY_SIZE(rgb_full_to_yuv); index++) {
		ret = gc573_mmio_write32(&dev->mmio, 0x10c0 + index * 4,
					 coefficients[index]);
		if (ret)
			return ret;
	}

	if (width != input_width || height != input_height) {
		/* Proven board_v4l2.c order after VIP/CSC setup: reset capture,
		 * restore full input window, V setup, H setup, enable scaling. */
		ret = gc573_mmio_write32(&dev->mmio, GC573_REG_CAPTURE_MODE, 0);
		if (ret)
			return ret;
		msleep(5);
		ret = gc573_mmio_write32(&dev->mmio, GC573_REG_CAPTURE_MODE, 3);
		if (ret)
			return ret;
		msleep(5);
		ret = gc573_mmio_write32(&dev->mmio, GC573_REG_VIP_INPUT_X, 0);
		if (!ret)
			ret = gc573_mmio_write32(&dev->mmio, GC573_REG_VIP_INPUT_WIDTH, input_width);
		if (!ret)
			ret = gc573_mmio_write32(&dev->mmio, GC573_REG_VIP_INPUT_Y, 0);
		if (!ret)
			ret = gc573_mmio_write32(&dev->mmio, GC573_REG_VIP_INPUT_HEIGHT, input_height);
		if (!ret)
			ret = gc573_scaler_setup(dev, input_width, input_height, width, height);
		if (!ret)
			ret = gc573_mmio_maskwrite32(&dev->mmio, GC573_REG_VIP_CONTROL, BIT(7), BIT(7));
		if (ret)
			return ret;
	}

	/* XV_*ScalerStart: enable auto-restart, read it back, then assert
	 * start while preserving only auto-restart. Native VIP bypasses cores
	 * (bit7 clear); retain the qualified native start behavior. */
	ret = gc573_mmio_write32(&dev->mmio, 0x60000, 0x80);
	if (!ret)
		ret = gc573_mmio_read32(&dev->mmio, 0x60000, &value);
	if (!ret)
		ret = gc573_mmio_write32(&dev->mmio, 0x60000, (value & 0x80) | 1);
	if (!ret)
		ret = gc573_mmio_write32(&dev->mmio, 0x40000, 0x80);
	if (!ret)
		ret = gc573_mmio_read32(&dev->mmio, 0x40000, &value);
	if (!ret)
		ret = gc573_mmio_write32(&dev->mmio, 0x40000, (value & 0x80) | 1);
	if (ret) {
		/* Best-effort rollback: no DMA has been armed at this point. */
		gc573_mmio_write32(&dev->mmio, 0x60000, 0);
		gc573_mmio_write32(&dev->mmio, 0x40000, 0);
		gc573_mmio_maskwrite32(&dev->mmio, GC573_REG_VIP_CONTROL, BIT(7), 0);
		return ret;
	}

	dev_info(&dev->pdev->dev, "VIP input %ux%u output %ux%u fourcc=%4.4s rate=%u/%u period=%u\n",
		 input_width, input_height, width, height, (char *)&pixelformat,
		 rate_num, rate_den, period);
	return 0;
}
