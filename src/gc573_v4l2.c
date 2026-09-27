// SPDX-License-Identifier: GPL-2.0-only
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/jiffies.h>
#include <linux/moduleparam.h>
#include <linux/workqueue.h>
#include <linux/list.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/videodev2.h>
#include <media/v4l2-ioctl.h>
#include <media/videobuf2-dma-sg.h>

#include "gc573_pure.h"
#include "gc573_bridge_recover.h"
#include "gc573_bridge_acquire.h"
#include "gc573_formats.h"
#include "gc573_audio.h"
#include "gc573_audio_dma.h"

#define GC573_DEFAULT_WIDTH	3840
#define GC573_DEFAULT_HEIGHT	2160
#define GC573_BUFFER_COUNT	8
/* Mode changes have produced stale black frames beyond a 16-frame drain
 * (1080p50 qualification). Drain 32 hardware frames before delivery.
 * This is bounded startup latency, not content-based black-frame filtering. */
#define GC573_STARTUP_FRAMES	32

/* Opt-in hardware diagnostic, once per binding; never enabled by default. */
static unsigned int test_hpd_after_ms;
module_param(test_hpd_after_ms, uint, 0444);
MODULE_PARM_DESC(test_hpd_after_ms,
	"Diagnostic: cycle upstream HPD once during capture after 1000..60000 ms (0 disables)");

#define GC573_LINK_POLL_MS 100
#define GC573_RECOVERY_RETRY_MS 1000

struct gc573_v4l2_ctx;

struct gc573_v4l2_buffer {
	struct vb2_v4l2_buffer vb;
	struct list_head list;
	struct gc573_v4l2_ctx *ctx;
	struct gc573_dma_segment *segments;
	void *cpu_addr;
	unsigned int num_segments;
	bool queued;
	bool in_hardware;
};

struct gc573_v4l2_ctx {
	struct gc573_device *dev;
	struct vb2_queue queue;
	struct mutex lock;
	spinlock_t buffer_lock;
	struct list_head pending;
	struct list_head in_hardware;
	struct v4l2_pix_format format;
	unsigned int interval_index;
	u32 sequence;
	u32 startup_frames;
	bool streaming;
	bool recovering; /* ctx->lock; VB2 stream remains active while paused */
	bool queue_error;
	struct delayed_work link_work;
	bool monitor_enabled; /* READ/WRITE_ONCE; cancellation under ctx->lock */
	bool test_hpd_done; /* ctx->lock; persists across STREAMOFF */
	unsigned long stream_started;
};

static const struct {
	u32 width;
	u32 height;
} gc573_sizes[] = {
	/* Native 4K input; FPGA delivers one of these output geometries. */
	{ 3840, 2160 },
	{ 1920, 1080 },
	{ 1280, 720 },
	{ 1280, 800 },
};

static const struct v4l2_fract gc573_intervals[] = {
	{ 1, 60 }, { 1001, 60000 }, { 1, 50 }, { 1, 30 },
	{ 1001, 30000 }, { 1, 25 }, { 1, 24 }, { 1001, 24000 },
};

static struct gc573_v4l2_ctx *gc573_ctx_from_queue(struct vb2_queue *q)
{
	return q->drv_priv;
}

static struct gc573_v4l2_buffer *gc573_buffer_from_vb(struct vb2_buffer *vb)
{
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);

	return container_of(vbuf, struct gc573_v4l2_buffer, vb);
}

static const u32 gc573_formats[] = {
	V4L2_PIX_FMT_YUYV, V4L2_PIX_FMT_NV12,
	V4L2_PIX_FMT_RGB24, V4L2_PIX_FMT_BGR24,
};

static void gc573_fill_pix_format(struct v4l2_pix_format *pix,
				  u32 width, u32 height, u32 fourcc)
{
	u32 stride, size;

	memset(pix, 0, sizeof(*pix));
	pix->width = width;
	pix->height = height;
	if (gc573_format_layout(fourcc, width, height, &stride, &size)) {
		fourcc = V4L2_PIX_FMT_YUYV;
		gc573_format_layout(fourcc, width, height, &stride, &size);
	}
	pix->pixelformat = fourcc;
	pix->field = V4L2_FIELD_NONE;
	pix->bytesperline = stride;
	pix->sizeimage = size;
	pix->colorspace = V4L2_COLORSPACE_REC709;
	pix->ycbcr_enc = V4L2_YCBCR_ENC_709;
	pix->quantization = (fourcc == V4L2_PIX_FMT_RGB24 ||
			     fourcc == V4L2_PIX_FMT_BGR24) ?
			    V4L2_QUANTIZATION_FULL_RANGE : V4L2_QUANTIZATION_LIM_RANGE;
	pix->xfer_func = V4L2_XFER_FUNC_709;
}

static int gc573_try_format(struct v4l2_format *f)
{
	struct v4l2_pix_format *pix = &f->fmt.pix;
	u32 fourcc = gc573_format_supported(pix->pixelformat) ?
		pix->pixelformat : V4L2_PIX_FMT_YUYV;
	unsigned int best = 0;
	u64 best_distance = U64_MAX;
	unsigned int i;

	if (f->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;

	/* Keep the distance calculation bounded for hostile ioctl dimensions. */
	pix->width = min_t(u32, pix->width, 8192);
	pix->height = min_t(u32, pix->height, 8192);

	for (i = 0; i < ARRAY_SIZE(gc573_sizes); i++) {
		s64 dx = (s64)pix->width - gc573_sizes[i].width;
		s64 dy = (s64)pix->height - gc573_sizes[i].height;
		u64 distance = dx * dx + dy * dy;

		if (distance < best_distance) {
			best = i;
			best_distance = distance;
		}
	}

	gc573_fill_pix_format(pix, gc573_sizes[best].width,
			      gc573_sizes[best].height, fourcc);
	return 0;
}

static int gc573_querycap(struct file *file, void *fh,
			  struct v4l2_capability *cap)
{
	struct gc573_v4l2_ctx *ctx = video_drvdata(file);

	strscpy(cap->driver, "gc573-pure", sizeof(cap->driver));
	strscpy(cap->card, "AVerMedia Live Gamer 4K", sizeof(cap->card));
	snprintf(cap->bus_info, sizeof(cap->bus_info), "PCI:%s",
		 pci_name(ctx->dev->pdev));
	cap->device_caps = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING;
	cap->capabilities = cap->device_caps | V4L2_CAP_DEVICE_CAPS;
	return 0;
}

static int gc573_enum_fmt(struct file *file, void *fh,
			  struct v4l2_fmtdesc *fmt)
{
	if (fmt->index >= ARRAY_SIZE(gc573_formats))
		return -EINVAL;
	fmt->pixelformat = gc573_formats[fmt->index];
	fmt->flags = fmt->pixelformat == V4L2_PIX_FMT_RGB24 ?
		V4L2_FMT_FLAG_EMULATED : 0;
	switch (fmt->pixelformat) {
	case V4L2_PIX_FMT_YUYV: strscpy(fmt->description, "YUYV 4:2:2", sizeof(fmt->description)); break;
	case V4L2_PIX_FMT_NV12: strscpy(fmt->description, "NV12 4:2:0", sizeof(fmt->description)); break;
	case V4L2_PIX_FMT_RGB24: strscpy(fmt->description, "RGB24", sizeof(fmt->description)); break;
	case V4L2_PIX_FMT_BGR24: strscpy(fmt->description, "BGR24", sizeof(fmt->description)); break;
	}
	return 0;
}

static int gc573_enum_framesizes(struct file *file, void *fh,
				 struct v4l2_frmsizeenum *fsize)
{
	if (!gc573_format_supported(fsize->pixel_format) ||
	    fsize->index >= ARRAY_SIZE(gc573_sizes))
		return -EINVAL;

	fsize->type = V4L2_FRMSIZE_TYPE_DISCRETE;
	fsize->discrete.width = gc573_sizes[fsize->index].width;
	fsize->discrete.height = gc573_sizes[fsize->index].height;
	return 0;
}

static int gc573_enum_frameintervals(struct file *file, void *fh,
				     struct v4l2_frmivalenum *fival)
{
	unsigned int i;

	if (!gc573_format_supported(fival->pixel_format) ||
	    fival->index >= ARRAY_SIZE(gc573_intervals))
		return -EINVAL;
	for (i = 0; i < ARRAY_SIZE(gc573_sizes); i++) {
		if (fival->width == gc573_sizes[i].width &&
		    fival->height == gc573_sizes[i].height) {
			fival->type = V4L2_FRMIVAL_TYPE_DISCRETE;
			fival->discrete = gc573_intervals[fival->index];
			return 0;
		}
	}
	return -EINVAL;
}

static int gc573_enum_input(struct file *file, void *fh,
			    struct v4l2_input *input)
{
	if (input->index)
		return -EINVAL;

	memset(input, 0, sizeof(*input));
	input->index = 0;
	strscpy(input->name, "HDMI", sizeof(input->name));
	input->type = V4L2_INPUT_TYPE_CAMERA;
	/* No analog-standard or DV-timing ioctl support is advertised. */
	input->capabilities = 0;
	return 0;
}

static int gc573_g_input(struct file *file, void *fh, unsigned int *input)
{
	*input = 0;
	return 0;
}

static int gc573_s_input(struct file *file, void *fh, unsigned int input)
{
	return input ? -EINVAL : 0;
}

static int gc573_g_parm(struct file *file, void *fh,
			struct v4l2_streamparm *parm)
{
	struct gc573_v4l2_ctx *ctx = video_drvdata(file);

	if (parm->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;

	memset(&parm->parm.capture, 0, sizeof(parm->parm.capture));
	parm->parm.capture.capability = V4L2_CAP_TIMEPERFRAME;
	parm->parm.capture.timeperframe = gc573_intervals[ctx->interval_index];
	return 0;
}

static int gc573_s_parm(struct file *file, void *fh,
			struct v4l2_streamparm *parm)
{
	struct gc573_v4l2_ctx *ctx = video_drvdata(file);
	struct v4l2_fract requested;
	u64 best_delta = U64_MAX, best_scale = 1;
	unsigned int best = 0, i;
	u64 req_num, req_den;

	if (parm->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;
	requested = parm->parm.capture.timeperframe;
	if (!requested.numerator || !requested.denominator) {
		best = 0;
	} else {
		req_num = requested.numerator;
		req_den = requested.denominator;
		for (i = 0; i < ARRAY_SIZE(gc573_intervals); i++) {
			u64 a = req_num * gc573_intervals[i].denominator;
			u64 b = (u64)gc573_intervals[i].numerator * req_den;
			u64 delta = a > b ? a - b : b - a;
			u64 scale = gc573_intervals[i].denominator;

			if (best_delta == U64_MAX ||
			    delta * best_scale < best_delta * scale) {
				best = i;
				best_delta = delta;
				best_scale = scale;
			}
		}
	}

	/* video_ioctl2 holds vdev->lock (ctx->lock) for this ioctl. */
	if (vb2_is_streaming(&ctx->queue))
		return -EBUSY;
	ctx->interval_index = best;

	return gc573_g_parm(file, fh, parm);
}

static int gc573_g_fmt(struct file *file, void *fh, struct v4l2_format *f)
{
	struct gc573_v4l2_ctx *ctx = video_drvdata(file);

	if (f->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;
	f->fmt.pix = ctx->format;
	return 0;
}

static int gc573_try_fmt(struct file *file, void *fh, struct v4l2_format *f)
{
	return gc573_try_format(f);
}

static int gc573_s_fmt(struct file *file, void *fh, struct v4l2_format *f)
{
	struct gc573_v4l2_ctx *ctx = video_drvdata(file);
	int ret;

	if (vb2_is_busy(&ctx->queue))
		return -EBUSY;
	ret = gc573_try_format(f);
	if (ret)
		return ret;
	ctx->format = f->fmt.pix;
	return 0;
}

static int gc573_queue_setup(struct vb2_queue *q,
			     unsigned int *num_buffers,
			     unsigned int *num_planes,
			     unsigned int sizes[],
			     struct device *alloc_devs[])
{
	struct gc573_v4l2_ctx *ctx = gc573_ctx_from_queue(q);

	if (*num_planes) {
		if (*num_planes != 1 || sizes[0] < ctx->format.sizeimage)
			return -EINVAL;
		return 0;
	}

	*num_planes = 1;
	sizes[0] = ctx->format.sizeimage;
	alloc_devs[0] = &ctx->dev->pdev->dev;
	if (!*num_buffers)
		*num_buffers = GC573_BUFFER_COUNT;
	return 0;
}

static int gc573_buf_prepare(struct vb2_buffer *vb)
{
	struct gc573_v4l2_buffer *buf = gc573_buffer_from_vb(vb);
	struct gc573_v4l2_ctx *ctx = buf->ctx;
	struct sg_table *table;
	struct scatterlist *sg;
	size_t remaining = ctx->format.sizeimage;
	unsigned int i, count = 0;

	if (vb2_plane_size(vb, 0) < ctx->format.sizeimage)
		return -EINVAL;
	buf->cpu_addr = NULL;
	if (ctx->format.pixelformat == V4L2_PIX_FMT_RGB24) {
		buf->cpu_addr = vb2_plane_vaddr(vb, 0);
		if (!buf->cpu_addr)
			return -EFAULT;
	}

	table = vb2_dma_sg_plane_desc(vb, 0);
	if (!table || !table->sgl || !table->nents)
		return -EINVAL;

	kfree(buf->segments);
	buf->segments = kcalloc(table->nents, sizeof(*buf->segments),
				GFP_KERNEL);
	if (!buf->segments)
		return -ENOMEM;

	for_each_sg(table->sgl, sg, table->nents, i) {
		size_t length = min_t(size_t, sg_dma_len(sg), remaining);

		if (!length)
			continue;
		buf->segments[count].dma_addr = sg_dma_address(sg);
		buf->segments[count].length_bytes = length;
		count++;
		remaining -= length;
		if (!remaining)
			break;
	}

	if (remaining || !count) {
		kfree(buf->segments);
		buf->segments = NULL;
		return -EINVAL;
	}

	buf->num_segments = count;
	vb2_set_plane_payload(vb, 0, ctx->format.sizeimage);
	return 0;
}

static void gc573_buf_finish(struct vb2_buffer *vb)
{
	struct gc573_v4l2_buffer *buf = gc573_buffer_from_vb(vb);
	struct gc573_v4l2_ctx *ctx = buf->ctx;

	if (vb->state != VB2_BUF_STATE_DONE ||
	    ctx->format.pixelformat != V4L2_PIX_FMT_RGB24)
		return;
	/* vb2 calls buf_finish after DMA ownership returns, in process context. */
	if (WARN_ON_ONCE(!buf->cpu_addr) ||
	    WARN_ON_ONCE(gc573_rgb24_swap_red_blue(buf->cpu_addr,
						   ctx->format.sizeimage)))
		return;
}

static void gc573_buf_cleanup(struct vb2_buffer *vb)
{
	struct gc573_v4l2_buffer *buf = gc573_buffer_from_vb(vb);

	kfree(buf->segments);
	buf->segments = NULL;
	buf->num_segments = 0;
}

static int gc573_buf_init(struct vb2_buffer *vb)
{
	struct gc573_v4l2_buffer *buf = gc573_buffer_from_vb(vb);

	buf->ctx = gc573_ctx_from_queue(vb->vb2_queue);
	INIT_LIST_HEAD(&buf->list);
	return 0;
}

static void gc573_buffer_return(struct gc573_v4l2_buffer *buf,
				enum vb2_buffer_state state)
{
	buf->queued = false;
	buf->in_hardware = false;
	buf->vb.vb2_buf.timestamp = ktime_get_ns();
	buf->vb.field = V4L2_FIELD_NONE;
	vb2_buffer_done(&buf->vb.vb2_buf, state);
}

/* Called with buffer_lock held; the hardware has four observed slots. */
static int gc573_refill_locked(struct gc573_v4l2_ctx *ctx)
{
	while (ctx->streaming && !ctx->queue_error &&
	       !list_empty(&ctx->pending)) {
		struct gc573_v4l2_buffer *buf;
		int ret;

		buf = list_first_entry(&ctx->pending,
				       struct gc573_v4l2_buffer, list);
		ret = gc573_hw_submit(ctx->dev, buf->segments,
				      buf->num_segments,
				      ctx->format.pixelformat == V4L2_PIX_FMT_NV12 ?
				      ctx->format.width * ctx->format.height : ctx->format.sizeimage,
				      ctx->format.pixelformat == V4L2_PIX_FMT_NV12 ?
				      ctx->format.width * ctx->format.height / 2 : 0, buf);
		if (ret == -ENOSPC)
			return 0;
		if (ret) {
			ctx->queue_error = true;
			return ret;
		}

		list_move_tail(&buf->list, &ctx->in_hardware);
		buf->in_hardware = true;
	}
	return 0;
}

/* Configure the fixed native input and the userspace-selected output. */
static int gc573_configure_capture(struct gc573_v4l2_ctx *ctx)
{
	u32 input_width = GC573_DEFAULT_WIDTH;
	u32 input_height = GC573_DEFAULT_HEIGHT;
	int ret;

	if (ctx->dev->bridge_source) {
		/* A healthy source can start video beside audio. Reconfiguration
		 * on a changed/lost input is not audio-transparent. Quiesce audio
		 * and report XRUN before touching the bridge in that case. */
		if (ctx->dev->audio_running &&
		    (gc573_bridge_check_capture(ctx->dev) ||
		     gc573_bridge_check_mode(ctx->dev))) {
			gc573_audio_dma_stop(ctx->dev);
			ctx->dev->audio_running = false;
			gc573_audio_xrun(ctx->dev->audio);
		}
		ret = gc573_bridge_prepare_capture(ctx->dev);
		if (ret)
			return ret;

		input_width = ctx->dev->input_width;
		input_height = ctx->dev->input_height;
		if (!input_width || !input_height)
			return -EAGAIN;
	}

	ret = ctx->dev->it6805->configure_capture(ctx->dev, input_width,
						 input_height, V4L2_PIX_FMT_YUYV);
	if (ret)
		return ret;

	return gc573_fpga_configure_capture(ctx->dev, input_width, input_height,
					    ctx->format.width,
					    ctx->format.height,
					    gc573_intervals[ctx->interval_index].denominator,
					    gc573_intervals[ctx->interval_index].numerator,
					    ctx->format.pixelformat);
}

static void gc573_buf_queue(struct vb2_buffer *vb)
{
	struct gc573_v4l2_buffer *buf = gc573_buffer_from_vb(vb);
	struct gc573_v4l2_ctx *ctx = buf->ctx;
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&ctx->buffer_lock, flags);
	buf->queued = true;
	buf->in_hardware = false;
	list_add_tail(&buf->list, &ctx->pending);
	ret = gc573_refill_locked(ctx);
	spin_unlock_irqrestore(&ctx->buffer_lock, flags);

	if (ret)
		vb2_queue_error(&ctx->queue);
}

/* ctx->lock serializes the worker with all process-side queue/bank access.
 * It MUST use trylock: STREAMOFF cancels work synchronously with this mutex
 * held. No I2C or sleeping operations execute in interrupt context. */
static void gc573_link_work(struct work_struct *work)
{
	struct gc573_v4l2_ctx *ctx = container_of(to_delayed_work(work),
					struct gc573_v4l2_ctx, link_work);
	struct gc573_v4l2_buffer *buf;
	unsigned long flags;
	int ret, loss_ret;

	if (!READ_ONCE(ctx->monitor_enabled))
		return;
	if (!mutex_trylock(&ctx->lock))
		goto reschedule;
	if (!READ_ONCE(ctx->monitor_enabled))
		goto unlock;

	if (test_hpd_after_ms >= 1000 && test_hpd_after_ms <= 60000 &&
	    !ctx->test_hpd_done && time_after_eq(jiffies, ctx->stream_started +
					      msecs_to_jiffies(test_hpd_after_ms))) {
		ctx->test_hpd_done = true;
		dev_info(&ctx->dev->pdev->dev, "active capture diagnostic HPD cycle begin\n");
		ret = gc573_bridge_test_hpd_cycle(ctx->dev);
		dev_info(&ctx->dev->pdev->dev, "active capture diagnostic HPD cycle end: %d\n", ret);
		if (ret)
			goto lost;
	}
	ret = gc573_bridge_check_capture(ctx->dev);
	if (!ret)
		ret = gc573_bridge_check_mode(ctx->dev);
	if (!ret && !ctx->recovering)
		goto unlock;
	if (!ret && ctx->recovering) {
		/* A successful check means the signal is back; configure below. */
		ret = -ENOLINK;
	}

lost:
	loss_ret = ret;
	if (!ctx->recovering) {
		/* Pause submissions, stop and synchronize DMA before recycling slots. */
		spin_lock_irqsave(&ctx->buffer_lock, flags);
		ctx->streaming = false;
		spin_unlock_irqrestore(&ctx->buffer_lock, flags);
		ret = gc573_hw_stop(ctx->dev);
		if (ret) {
			spin_lock_irqsave(&ctx->buffer_lock, flags);
			ctx->queue_error = true;
			spin_unlock_irqrestore(&ctx->buffer_lock, flags);
			WRITE_ONCE(ctx->monitor_enabled, false);
			vb2_queue_error(&ctx->queue);
			dev_err(&ctx->dev->pdev->dev,
				"DMA stop failed during link recovery: %d\n", ret);
			goto unlock;
		}
		spin_lock_irqsave(&ctx->buffer_lock, flags);
		list_splice_tail_init(&ctx->in_hardware, &ctx->pending);
		list_for_each_entry(buf, &ctx->pending, list)
			buf->in_hardware = false;
		spin_unlock_irqrestore(&ctx->buffer_lock, flags);
		ctx->recovering = true;
		dev_warn(&ctx->dev->pdev->dev,
			 "capture paused for input recovery (status=%d)\n", loss_ret);
	}

	ret = gc573_configure_capture(ctx);
	if (ret)
		goto unlock;

	spin_lock_irqsave(&ctx->buffer_lock, flags);
	ctx->queue_error = false;
	ctx->startup_frames = GC573_STARTUP_FRAMES;
	ctx->streaming = true;
	ret = gc573_refill_locked(ctx);
	spin_unlock_irqrestore(&ctx->buffer_lock, flags);
	if (ret)
		goto fatal_restart;
	ret = gc573_hw_start(ctx->dev);
	if (ret)
		goto fatal_restart;
	ctx->recovering = false;
	dev_info(&ctx->dev->pdev->dev,
		 "capture input recovered; draining %u startup frames\n",
		 GC573_STARTUP_FRAMES);
	goto unlock;

fatal_restart:
	spin_lock_irqsave(&ctx->buffer_lock, flags);
	ctx->streaming = false;
	ctx->queue_error = true;
	spin_unlock_irqrestore(&ctx->buffer_lock, flags);
	gc573_hw_stop(ctx->dev);
	WRITE_ONCE(ctx->monitor_enabled, false);
	vb2_queue_error(&ctx->queue);
	dev_err(&ctx->dev->pdev->dev,
		"DMA restart failed during link recovery: %d\n", ret);
unlock:
	mutex_unlock(&ctx->lock);
reschedule:
	if (READ_ONCE(ctx->monitor_enabled))
		schedule_delayed_work(&ctx->link_work,
				      msecs_to_jiffies(READ_ONCE(ctx->recovering) ?
					GC573_RECOVERY_RETRY_MS : GC573_LINK_POLL_MS));
}

static int gc573_start_streaming(struct vb2_queue *q, unsigned int count)
{
	struct gc573_v4l2_ctx *ctx = gc573_ctx_from_queue(q);
	LIST_HEAD(return_list);
	struct gc573_v4l2_buffer *buf, *tmp;
	unsigned long flags;
	int ret;

	(void)count;

	/* VB2 holds ctx->lock and DMA is stopped: one bank/bridge owner. */
	ret = gc573_configure_capture(ctx);
	if (ret)
		goto err_return_buffers;

	spin_lock_irqsave(&ctx->buffer_lock, flags);
	ctx->queue_error = false;
	ctx->sequence = 0;
	ctx->startup_frames = GC573_STARTUP_FRAMES;
	ctx->streaming = true;
	ret = gc573_refill_locked(ctx);
	spin_unlock_irqrestore(&ctx->buffer_lock, flags);
	if (ret)
		goto err_return_buffers;

	ret = gc573_hw_start(ctx->dev);
	if (!ret) {
		if (ctx->dev->bridge_source) {
			ctx->recovering = false;
			ctx->stream_started = jiffies;
			WRITE_ONCE(ctx->monitor_enabled, true);
			schedule_delayed_work(&ctx->link_work,
					      msecs_to_jiffies(GC573_LINK_POLL_MS));
		}
		return 0;
	}

err_return_buffers:
	spin_lock_irqsave(&ctx->buffer_lock, flags);
	ctx->streaming = false;
	spin_unlock_irqrestore(&ctx->buffer_lock, flags);

	gc573_hw_stop(ctx->dev);
	spin_lock_irqsave(&ctx->buffer_lock, flags);
	list_splice_init(&ctx->pending, &return_list);
	list_splice_tail_init(&ctx->in_hardware, &return_list);
	list_for_each_entry(buf, &return_list, list) {
		buf->queued = false;
		buf->in_hardware = false;
	}
	spin_unlock_irqrestore(&ctx->buffer_lock, flags);

	/* vb2 requires buffers from a failed start to return to its queue. */
	list_for_each_entry_safe(buf, tmp, &return_list, list) {
		list_del_init(&buf->list);
		gc573_buffer_return(buf, VB2_BUF_STATE_QUEUED);
	}
	return ret;
}

static void gc573_stop_streaming(struct vb2_queue *q)
{
	struct gc573_v4l2_ctx *ctx = gc573_ctx_from_queue(q);
	LIST_HEAD(return_list);
	struct gc573_v4l2_buffer *buf, *tmp;
	unsigned long flags;

	WRITE_ONCE(ctx->monitor_enabled, false);
	cancel_delayed_work_sync(&ctx->link_work);
	ctx->recovering = false;

	spin_lock_irqsave(&ctx->buffer_lock, flags);
	ctx->streaming = false;
	spin_unlock_irqrestore(&ctx->buffer_lock, flags);

	gc573_hw_stop(ctx->dev);

	spin_lock_irqsave(&ctx->buffer_lock, flags);
	list_splice_init(&ctx->pending, &return_list);
	list_splice_tail_init(&ctx->in_hardware, &return_list);
	list_for_each_entry(buf, &return_list, list) {
		buf->queued = false;
		buf->in_hardware = false;
	}
	spin_unlock_irqrestore(&ctx->buffer_lock, flags);

	list_for_each_entry_safe(buf, tmp, &return_list, list) {
		list_del_init(&buf->list);
		gc573_buffer_return(buf, VB2_BUF_STATE_ERROR);
	}
}

static const struct vb2_ops gc573_vb2_ops = {
	.queue_setup = gc573_queue_setup,
	.buf_init = gc573_buf_init,
	.buf_prepare = gc573_buf_prepare,
	.buf_finish = gc573_buf_finish,
	.buf_cleanup = gc573_buf_cleanup,
	.buf_queue = gc573_buf_queue,
	.start_streaming = gc573_start_streaming,
	.stop_streaming = gc573_stop_streaming,
	.wait_prepare = vb2_ops_wait_prepare,
	.wait_finish = vb2_ops_wait_finish,
};

static const struct v4l2_ioctl_ops gc573_ioctl_ops = {
	.vidioc_querycap = gc573_querycap,
	.vidioc_enum_fmt_vid_cap = gc573_enum_fmt,
	.vidioc_enum_framesizes = gc573_enum_framesizes,
	.vidioc_enum_frameintervals = gc573_enum_frameintervals,
	.vidioc_enum_input = gc573_enum_input,
	.vidioc_g_input = gc573_g_input,
	.vidioc_s_input = gc573_s_input,
	.vidioc_g_parm = gc573_g_parm,
	.vidioc_s_parm = gc573_s_parm,
	.vidioc_g_fmt_vid_cap = gc573_g_fmt,
	.vidioc_try_fmt_vid_cap = gc573_try_fmt,
	.vidioc_s_fmt_vid_cap = gc573_s_fmt,
	.vidioc_reqbufs = vb2_ioctl_reqbufs,
	.vidioc_create_bufs = vb2_ioctl_create_bufs,
	.vidioc_prepare_buf = vb2_ioctl_prepare_buf,
	.vidioc_querybuf = vb2_ioctl_querybuf,
	.vidioc_qbuf = vb2_ioctl_qbuf,
	.vidioc_dqbuf = vb2_ioctl_dqbuf,
	.vidioc_expbuf = vb2_ioctl_expbuf,
	.vidioc_streamon = vb2_ioctl_streamon,
	.vidioc_streamoff = vb2_ioctl_streamoff,
};

static const struct v4l2_file_operations gc573_fops = {
	.owner = THIS_MODULE,
	.open = v4l2_fh_open,
	.release = vb2_fop_release,
	.read = vb2_fop_read,
	.poll = vb2_fop_poll,
	.mmap = vb2_fop_mmap,
	.unlocked_ioctl = video_ioctl2,
	.compat_ioctl32 = video_ioctl2,
};

static void gc573_video_release(struct video_device *vdev)
{
	struct gc573_v4l2_ctx *ctx = video_get_drvdata(vdev);

	WARN_ON_ONCE(READ_ONCE(ctx->monitor_enabled));
	WRITE_ONCE(ctx->monitor_enabled, false);
	cancel_delayed_work_sync(&ctx->link_work);
	kfree(ctx);
	video_device_release(vdev);
}

int gc573_v4l2_register(struct gc573_device *dev)
{
	struct gc573_v4l2_ctx *ctx;
	struct video_device *vdev;
	int ret;

	if (test_hpd_after_ms &&
	    (test_hpd_after_ms < 1000 || test_hpd_after_ms > 60000))
		return -EINVAL;

	ret = v4l2_device_register(&dev->pdev->dev, &dev->v4l2_dev);
	if (ret)
		return ret;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx) {
		ret = -ENOMEM;
		goto err_v4l2;
	}
	ctx->dev = dev;
	mutex_init(&ctx->lock);
	INIT_DELAYED_WORK(&ctx->link_work, gc573_link_work);
	spin_lock_init(&ctx->buffer_lock);
	INIT_LIST_HEAD(&ctx->pending);
	INIT_LIST_HEAD(&ctx->in_hardware);
	gc573_fill_pix_format(&ctx->format, GC573_DEFAULT_WIDTH,
			      GC573_DEFAULT_HEIGHT, V4L2_PIX_FMT_YUYV);

	ctx->queue.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	ctx->queue.io_modes = VB2_MMAP | VB2_DMABUF | VB2_READ;
	ctx->queue.drv_priv = ctx;
	ctx->queue.buf_struct_size = sizeof(struct gc573_v4l2_buffer);
	ctx->queue.ops = &gc573_vb2_ops;
	ctx->queue.mem_ops = &vb2_dma_sg_memops;
	ctx->queue.timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
	ctx->queue.lock = &ctx->lock;
	ctx->queue.dev = &dev->pdev->dev;
	ctx->queue.min_queued_buffers = 1;
	ctx->queue.gfp_flags = GFP_DMA32;
	ret = vb2_queue_init(&ctx->queue);
	if (ret)
		goto err_ctx;

	vdev = video_device_alloc();
	if (!vdev) {
		ret = -ENOMEM;
		goto err_queue;
	}
	strscpy(vdev->name, "gc573-pure", sizeof(vdev->name));
	vdev->v4l2_dev = &dev->v4l2_dev;
	vdev->fops = &gc573_fops;
	vdev->ioctl_ops = &gc573_ioctl_ops;
	vdev->release = gc573_video_release;
	vdev->device_caps = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING;
	vdev->lock = &ctx->lock;
	vdev->queue = &ctx->queue;
	video_set_drvdata(vdev, ctx);
	dev->vdev = vdev;

	ret = video_register_device(vdev, VFL_TYPE_VIDEO, -1);
	if (ret)
		goto err_video;

	return 0;

err_video:
	dev->vdev = NULL;
	video_device_release(vdev);
err_queue:
	vb2_queue_release(&ctx->queue);
err_ctx:
	kfree(ctx);
err_v4l2:
	v4l2_device_unregister(&dev->v4l2_dev);
	return ret;
}

void gc573_v4l2_unregister(struct gc573_device *dev)
{
	if (dev->vdev) {
		vb2_video_unregister_device(dev->vdev);
		dev->vdev = NULL;
	}
	v4l2_device_unregister(&dev->v4l2_dev);
}

void gc573_v4l2_buffer_done(struct gc573_device *dev, void *cookie)
{
	struct gc573_v4l2_buffer *buf = cookie;
	struct gc573_v4l2_ctx *ctx;
	unsigned long flags;
	int ret;

	if (!dev || !buf)
		return;
	ctx = buf->ctx;
	if (!ctx || ctx->dev != dev)
		return;

	spin_lock_irqsave(&ctx->buffer_lock, flags);
	if (!buf->in_hardware) {
		spin_unlock_irqrestore(&ctx->buffer_lock, flags);
		return;
	}
	list_del_init(&buf->list);
	buf->in_hardware = false;
	/* Teardown owns the vb2 return state: ERROR after stop, QUEUED after
	 * a failed start. Keep racing completions on its cleanup list. */
	if (!ctx->streaming) {
		list_add_tail(&buf->list, &ctx->pending);
		spin_unlock_irqrestore(&ctx->buffer_lock, flags);
		return;
	}
	if (ctx->streaming && ctx->startup_frames) {
		bool ready = !--ctx->startup_frames;

		list_add_tail(&buf->list, &ctx->pending);
		ret = gc573_refill_locked(ctx);
		spin_unlock_irqrestore(&ctx->buffer_lock, flags);
		if (ready)
			dev_info(&dev->pdev->dev,
				 "startup drain complete: %u hardware frames\n",
				 GC573_STARTUP_FRAMES);
		if (ret)
			vb2_queue_error(&ctx->queue);
		return;
	}
	buf->queued = false;
	buf->vb.sequence = ctx->sequence++;
	ret = gc573_refill_locked(ctx);
	spin_unlock_irqrestore(&ctx->buffer_lock, flags);

	if (READ_ONCE(gc573_trace_frames))
		dev_info(&dev->pdev->dev, "completed capture buffer sequence=%u\n",
			 buf->vb.sequence);
	gc573_buffer_return(buf, VB2_BUF_STATE_DONE);
	if (ret)
		vb2_queue_error(&ctx->queue);
}
