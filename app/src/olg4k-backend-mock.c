/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Deterministic in-process V4L2 backend for offline tests and CI.
 *
 * Implements the exact userspace contract of the gc573-pure driver:
 * the same formats, sizes, intervals, normalization, and a working
 * streaming loop with a deterministic test pattern. No /dev access.
 *
 * State is per open() (fd token), so two open devices do not interfere.
 */
#include "olg4k/backend.h"

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/poll.h>

/* ---- driver tables (mirrors src/gc573_v4l2.c + src/gc573_formats.h) ---- */

static const __u32 mock_formats[4] = {
	V4L2_PIX_FMT_YUYV, V4L2_PIX_FMT_NV12,
	V4L2_PIX_FMT_RGB24, V4L2_PIX_FMT_BGR24,
};

struct mock_size { unsigned int width, height; };

static const struct mock_size mock_sizes[4] = {
	{ 3840, 2160 }, { 1920, 1080 }, { 1280, 720 }, { 1280, 800 },
};

static const struct v4l2_fract mock_intervals[8] = {
	{ 1, 60 }, { 1001, 60000 }, { 1, 50 }, { 1, 30 },
	{ 1001, 30000 }, { 1, 25 }, { 1, 24 }, { 1001, 24000 },
};

static int mock_format_supported(__u32 fourcc)
{
	return fourcc == V4L2_PIX_FMT_YUYV || fourcc == V4L2_PIX_FMT_NV12 ||
	       fourcc == V4L2_PIX_FMT_RGB24 || fourcc == V4L2_PIX_FMT_BGR24;
}

static int mock_format_layout(__u32 fourcc, unsigned int width,
	unsigned int height, unsigned int *bytesperline, unsigned int *sizeimage)
{
	unsigned int stride = 0, size = 0;
	if (!width || !height || (width & 1) || (height & 1) ||
	    !mock_format_supported(fourcc))
		return -EINVAL;
	switch (fourcc) {
		case V4L2_PIX_FMT_YUYV:
			stride = width * 2U;
			size = stride * height;
			break;
		case V4L2_PIX_FMT_NV12:
				size = width * height;
				size += size / 2;
				stride = width;
				break;
		case V4L2_PIX_FMT_RGB24:
		case V4L2_PIX_FMT_BGR24:
				stride = width * 3U;
				size = stride * height;
				break;
		default:
			return -EINVAL;
	}
	*bytesperline = stride;
	*sizeimage = size;
	return 0;
}

static const char *mock_fmt_desc(__u32 fourcc)
{
	switch (fourcc) {
		case V4L2_PIX_FMT_YUYV: return "YUYV 4:2:2";
		case V4L2_PIX_FMT_NV12: return "NV12 4:2:0";
		case V4L2_PIX_FMT_RGB24: return "RGB24";
		case V4L2_PIX_FMT_BGR24: return "BGR24";
		default: return "unknown";
	}
}

/* ---- per-device state ---- */

#define MOCK_MAX_BUFS 16

struct mock_buf {
	bool queued;
	bool done;
	unsigned long long index;
	unsigned int bytesused;
	unsigned int flags;
	struct timeval timestamp;
};

struct mock_dev {
	struct v4l2_pix_format format;
	unsigned int interval_index;
	bool streaming;
	unsigned int nbufs;
	struct mock_buf bufs[MOCK_MAX_BUFS];
	unsigned long long frame_no;
	pthread_mutex_t lock;
	int refcount;
};

#define MOCK_MAX_OPEN 8

static struct mock_dev *mock_table[MOCK_MAX_OPEN];
static int mock_next_handle;

static struct mock_dev *mock_alloc(void)
{
	struct mock_dev *d = calloc(1, sizeof(*d));
	if (!d)
		return NULL;
	pthread_mutex_init(&d->lock, NULL);
	d->refcount = 1;
	return d;
}

static void mock_free(struct mock_dev *d)
{
	pthread_mutex_destroy(&d->lock);
	free(d);
}

/* ---- open / close (fd is the impl pointer in the mock) ---- */

static int mock_open(const char *path, void *impl, Olg4kErr *err, int *out_fd)
{
	(void)path;
	(void)impl;
	struct mock_dev *d = mock_alloc();
	int h;
	if (!d) {
		*err = OLG4K_ERR_OPEN;
		*out_fd = ENOMEM;
		return -1;
	}
	/* small integer handle into the mock table (fd is int in the API) */
	for (h = 1; h <= MOCK_MAX_OPEN; h++)
		if (!mock_table[h])
			break;
	if (h > MOCK_MAX_OPEN) {
		mock_free(d);
		*err = OLG4K_ERR_OPEN;
		*out_fd = ENFILE;
		return -1;
	}
	mock_table[h] = d;
	(void)mock_next_handle;
	/* default format: 1920x1080 YUYV 60fps (driver default) */
	unsigned int stride, size;
	mock_format_layout(V4L2_PIX_FMT_YUYV, 1920, 1080, &stride, &size);
	d->format.width = 1920;
	d->format.height = 1080;
	d->format.pixelformat = V4L2_PIX_FMT_YUYV;
	d->format.bytesperline = stride;
	d->format.sizeimage = size;
	d->format.field = V4L2_FIELD_NONE;
	d->format.colorspace = V4L2_COLORSPACE_REC709;
	d->format.xfer_func = V4L2_XFER_FUNC_709;
	d->format.quantization = V4L2_QUANTIZATION_LIM_RANGE;
	d->interval_index = 0;
	*out_fd = h;
	*err = OLG4K_ERR_OK;
	return 0;
}

static struct mock_dev *mock_handle(int fd)
{
	if (fd < 1 || fd > MOCK_MAX_OPEN)
		return NULL;
	return mock_table[fd];
}

static int mock_close(int fd, void *impl)
{
	(void)impl;
	struct mock_dev *d = mock_handle(fd);
	if (!d)
		return 0;
	d->refcount--;
	if (d->refcount <= 0) {
		mock_table[fd] = NULL;
		mock_free(d);
	}
	return 0;
}

/* ---- normalization: exact mirror of gc573_try_format() ---- */

static int mock_try_format(struct v4l2_format *f)
{
	struct v4l2_pix_format *pix = &f->fmt.pix;
	__u32 fourcc = mock_format_supported(pix->pixelformat) ?
		pix->pixelformat : V4L2_PIX_FMT_YUYV;
	unsigned int best = 0;
	unsigned long long best_distance = ~0ULL;
	unsigned int i;
	if (f->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;
	if (pix->width > 8192)
		pix->width = 8192;
	if (pix->height > 8192)
		pix->height = 8192;
	for (i = 0; i < 4; i++) {
		long long dx = (long long)pix->width - mock_sizes[i].width;
		long long dy = (long long)pix->height - mock_sizes[i].height;
		unsigned long long distance = (unsigned long long)(dx * dx + dy * dy);
		if (distance < best_distance) {
			best = i;
			best_distance = distance;
		}
	}
	unsigned int stride = 0, size = 0;
	mock_format_layout(fourcc, mock_sizes[best].width, mock_sizes[best].height,
	                    &stride, &size);
	memset(pix, 0, sizeof(*pix));
	pix->width = mock_sizes[best].width;
	pix->height = mock_sizes[best].height;
	pix->pixelformat = fourcc;
	pix->field = V4L2_FIELD_NONE;
	pix->bytesperline = stride;
	pix->sizeimage = size;
	pix->colorspace = V4L2_COLORSPACE_REC709;
	pix->ycbcr_enc = V4L2_YCBCR_ENC_709;
	pix->xfer_func = V4L2_XFER_FUNC_709;
	pix->quantization = (fourcc == V4L2_PIX_FMT_RGB24 ||
	                      fourcc == V4L2_PIX_FMT_BGR24) ?
	                     V4L2_QUANTIZATION_FULL_RANGE :
	                     V4L2_QUANTIZATION_LIM_RANGE;
	return 0;
}


/* ---- test pattern fill (deterministic, no RNG) ---- */

static void mock_fill_frame(struct mock_dev *d, unsigned char *dst,
	size_t len, unsigned long long frame_no)
{
	(void)d;
	size_t i;
	/* Diagonal ramp: each byte = (x + frame) mod 256 so every frame differs. */
	for (i = 0; i < len; i++)
		dst[i] = (unsigned char)((i + frame_no * 17) & 0xFF);
}

/* ---- buffer accounting ---- */

static struct mock_buf *mock_next_done(struct mock_dev *d)
{
	unsigned int i;
	for (i = 0; i < d->nbufs; i++)
		if (d->bufs[i].done)
			return &d->bufs[i];
	return NULL;
}

/* Produce one frame if the hardware queue has a free slot (4 slots). */

static void mock_tick_locked(struct mock_dev *d)
{
	unsigned int in_hw = 0;
	unsigned int queued = 0;
	unsigned int i;
	if (!d->streaming)
		return;
	for (i = 0; i < d->nbufs; i++) {
		if (d->bufs[i].queued)
			queued++;
	}
	/* The mock retires one queued buffer per tick up to the 4-slot limit. */
	if (queued == 0)
		return;
	(void)in_hw;
	for (i = 0; i < d->nbufs && in_hw < 4; i++) {
		if (d->bufs[i].queued) {
			struct mock_buf *b = &d->bufs[i];
			b->queued = false;
			b->done = true;
			b->index = d->frame_no;
			b->bytesused = d->format.sizeimage;
			b->flags = 0;
			b->timestamp.tv_sec = (time_t)(d->frame_no / 60);
			b->timestamp.tv_usec = (suseconds_t)((d->frame_no % 60) * 100000 / 60);
			d->frame_no++;
			in_hw++;
		}
	}
}

/* ---- ioctl dispatch ---- */

static long mock_ioctl(int fd, unsigned long request, void *arg, void *impl,
	int *err_out)
{
	(void)impl;
	struct mock_dev *d = mock_handle(fd);
	if (!d) {
		*err_out = EBADF;
		return -1;
	}
	long ret = 0;
	*err_out = 0;
	pthread_mutex_lock(&d->lock);
	switch (request) {
		case VIDIOC_QUERYCAP: {
			struct v4l2_capability *c = arg;
			memset(c, 0, sizeof(*c));
			memcpy(c->driver, "gc573-pure", sizeof("gc573-pure"));
			memcpy(c->card, "AVerMedia Live Gamer 4K", sizeof("AVerMedia Live Gamer 4K"));
			memcpy(c->bus_info, "PCI:0000:01:00.0", sizeof("PCI:0000:01:00.0"));
			c->device_caps = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING;
			c->capabilities = c->device_caps | V4L2_CAP_DEVICE_CAPS;
			break;
		}
		case VIDIOC_ENUM_FMT: {
			struct v4l2_fmtdesc *f = arg;
			if (f->index >= 4) { ret = -1; *err_out = EINVAL; break; }
			f->pixelformat = mock_formats[f->index];
			f->flags = f->pixelformat == V4L2_PIX_FMT_RGB24 ?
				V4L2_FMT_FLAG_EMULATED : 0;
			memcpy(f->description, mock_fmt_desc(f->pixelformat),
				sizeof("YUYV 4:2:2"));
			break;
		}
		case VIDIOC_ENUM_FRAMESIZES: {
			struct v4l2_frmsizeenum *fs = arg;
			if (!mock_format_supported(fs->pixel_format) || fs->index >= 4) {
				ret = -1; *err_out = EINVAL; break;
			}
			fs->type = V4L2_FRMSIZE_TYPE_DISCRETE;
			fs->discrete.width = mock_sizes[fs->index].width;
			fs->discrete.height = mock_sizes[fs->index].height;
			break;
		}
		case VIDIOC_ENUM_FRAMEINTERVALS: {
			struct v4l2_frmivalenum *fi = arg;
			unsigned int i;
			bool found = false;
			if (!mock_format_supported(fi->pixel_format) || fi->index >= 8) {
				ret = -1; *err_out = EINVAL; break;
			}
			for (i = 0; i < 4; i++) {
				if (fi->width == mock_sizes[i].width &&
					fi->height == mock_sizes[i].height) {
					found = true;
					break;
				}
			}
			if (!found) { ret = -1; *err_out = EINVAL; break; }
			fi->type = V4L2_FRMIVAL_TYPE_DISCRETE;
			fi->discrete = mock_intervals[fi->index];
			break;
		}
		case VIDIOC_ENUMINPUT: {
			struct v4l2_input *in = arg;
			if (in->index != 0) { ret = -1; *err_out = EINVAL; break; }
			memset(in, 0, sizeof(*in));
			in->index = 0;
			memcpy(in->name, "HDMI", 5);
			in->type = V4L2_INPUT_TYPE_CAMERA;
			break;
		}
		case VIDIOC_G_INPUT: {
			*(unsigned int *)arg = 0;
			break;
		}
		case VIDIOC_S_INPUT: {
			unsigned int *in = arg;
			if (*in != 0) { ret = -1; *err_out = EINVAL; break; }
			break;
		}
		case VIDIOC_G_FMT: {
			struct v4l2_format *f = arg;
			if (f->type != V4L2_BUF_TYPE_VIDEO_CAPTURE) {
				ret = -1; *err_out = EINVAL; break;
			}
			f->fmt.pix = d->format;
			break;
		}
		case VIDIOC_TRY_FMT: {
			struct v4l2_format *f = arg;
			int e = mock_try_format(f);
			if (e) { ret = -1; *err_out = -e; }
			break;
		}
		case VIDIOC_S_FMT: {
			struct v4l2_format *f = arg;
			int e = mock_try_format(f);
			if (e) { ret = -1; *err_out = -e; break; }
			d->format = f->fmt.pix;
			break;
		}
		case VIDIOC_G_PARM: {
			struct v4l2_streamparm *p = arg;
			if (p->type != V4L2_BUF_TYPE_VIDEO_CAPTURE) {
				ret = -1; *err_out = EINVAL; break;
			}
			memset(&p->parm.capture, 0, sizeof(p->parm.capture));
			p->parm.capture.capability = V4L2_CAP_TIMEPERFRAME;
			p->parm.capture.timeperframe = mock_intervals[d->interval_index];
			break;
		}
		case VIDIOC_S_PARM: {
			struct v4l2_streamparm *p = arg;
			struct v4l2_fract req = p->parm.capture.timeperframe;
			unsigned int best = 0;
			unsigned long long best_delta = ~0ULL, best_scale = 1;
			unsigned int i;
			if (p->type != V4L2_BUF_TYPE_VIDEO_CAPTURE) {
				ret = -1; *err_out = EINVAL; break;
			}
			if (!req.numerator || !req.denominator) {
				best = 0;
			} else {
				for (i = 0; i < 8; i++) {
					unsigned long long a2 = (unsigned long long)req.numerator *
						mock_intervals[i].denominator;
						unsigned long long b2 = (unsigned long long)
						mock_intervals[i].numerator * req.denominator;
						unsigned long long delta = a2 > b2 ? a2 - b2 : b2 - a2;
						unsigned long long scale = mock_intervals[i].denominator;
						if (best_delta == ~0ULL || delta * best_scale < best_delta * scale) {
							best = i;
							best_delta = delta;
							best_scale = scale;
						}
				}
			}
			d->interval_index = best;
			p->parm.capture.timeperframe = mock_intervals[best];
			break;
		}
		case VIDIOC_REQBUFS: {
			struct v4l2_requestbuffers *rb = arg;
			if (rb->count > MOCK_MAX_BUFS) { rb->count = MOCK_MAX_BUFS; }
			if (rb->count < 1) { ret = -1; *err_out = EINVAL; break; }
			d->nbufs = rb->count;
			memset(d->bufs, 0, sizeof(d->bufs));
			rb->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
			rb->memory = V4L2_MEMORY_MMAP;
			break;
		}
		case VIDIOC_QUERYBUF: {
			struct v4l2_buffer *b = arg;
			if (b->index >= d->nbufs) { ret = -1; *err_out = EINVAL; break; }
			memset(b, 0, sizeof(*b));
			b->index = d->bufs[b->index].index;
			b->length = d->format.sizeimage;
			b->bytesused = d->bufs[b->index].bytesused;
			b->flags = d->bufs[b->index].flags;
			b->timestamp = d->bufs[b->index].timestamp;
			b->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
			b->memory = V4L2_MEMORY_MMAP;
			b->m.offset = (unsigned int)(b->index * d->format.sizeimage);
			break;
		}
		case VIDIOC_QBUF: {
			struct v4l2_buffer *b = arg;
			if (b->index >= d->nbufs) {
				ret = -1; *err_out = EINVAL; break;
			}
			d->bufs[b->index].queued = true;
			d->bufs[b->index].done = false;
			mock_tick_locked(d);
			break;
		}
		case VIDIOC_DQBUF: {
			struct v4l2_buffer *b = arg;
			struct mock_buf *mb;
			unsigned int idx;
			mock_tick_locked(d);
			mb = mock_next_done(d);
			if (!mb) { ret = -1; *err_out = EAGAIN; break; }
			for (idx = 0; idx < d->nbufs; idx++)
				if (&d->bufs[idx] == mb)
					break;
			memset(b, 0, sizeof(*b));
			b->index = idx;
			b->length = d->format.sizeimage;
			b->bytesused = mb->bytesused;
			b->flags = mb->flags;
			b->timestamp = mb->timestamp;
			b->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
			b->memory = V4L2_MEMORY_MMAP;
			b->m.offset = (unsigned int)(idx * d->format.sizeimage);
			mb->done = false;
			break;
		}
		case VIDIOC_STREAMON: {
			enum v4l2_buf_type *t = arg;
			if (*t != V4L2_BUF_TYPE_VIDEO_CAPTURE) {
				ret = -1; *err_out = EINVAL; break;
			}
			if (d->nbufs == 0) { ret = -1; *err_out = ENODATA; break; }
			d->streaming = true;
			d->frame_no = 0;
			break;
		}
		case VIDIOC_STREAMOFF: {
			enum v4l2_buf_type *t = arg;
			if (*t != V4L2_BUF_TYPE_VIDEO_CAPTURE) {
				ret = -1; *err_out = EINVAL; break;
			}
			d->streaming = false;
			break;
		}
		default:
			ret = -1;
			*err_out = ENOTTY;
			break;
	}
	pthread_mutex_unlock(&d->lock);
	return ret;
}

/* ---- mmap: serve the per-device frame pool from a single arena ---- */

static int mock_mmap(int fd, void **addr, size_t length, int prot, int flags,
	off_t offset, void *impl, Olg4kErr *err, int *err_out)
{
	(void)impl;
	struct mock_dev *d = mock_handle(fd);
	if (!d) {
		*err_out = EBADF;
		*err = OLG4K_ERR_BACKEND;
		return -1;
	}
	size_t frame = (size_t)(offset / d->format.sizeimage);
	void *base;
	if (!length || frame >= d->nbufs || length < d->format.sizeimage) {
		*err_out = EINVAL; *err = OLG4K_ERR_BACKEND; return -1;
	}
	pthread_mutex_lock(&d->lock);
	base = mmap(NULL, length, prot,
	            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (base == MAP_FAILED) {
		int e = errno;
		pthread_mutex_unlock(&d->lock);
		*err_out = e;
		*err = OLG4K_ERR_BACKEND;
		return -1;
	}
	/* Deterministic pattern so unit tests can check content. */
	mock_fill_frame(d, (unsigned char *)base, length, 1);
	pthread_mutex_unlock(&d->lock);
	(void)flags;
	*addr = base;
	return 0;
}

static int mock_munmap(void *addr, size_t length, void *impl)
{
	(void)impl;
	return munmap(addr, length);
}

static int mock_poll(int fd, void *impl, struct pollfd *fds, nfds_t nfds,
	int timeout_ms)
{
	(void)impl;
	(void)fds;
	struct mock_dev *d = mock_handle(fd);
	if (!d)
		return -1;
	int i;
	int ready = 0;
	pthread_mutex_lock(&d->lock);
	mock_tick_locked(d);
	for (i = 0; i < (int)nfds; i++)
		if (mock_next_done(d))
			ready++;
	pthread_mutex_unlock(&d->lock);
	if (ready > 0)
		return ready;
	if (timeout_ms < 0)
		return -1; /* would block: signal EAGAIN via errno */
	return 0;
}

const Olg4kBackend olg4k_backend_mock = {
	.open = mock_open,
	.ioctl = mock_ioctl,
	.close = mock_close,
	.mmap = mock_mmap,
	.munmap = mock_munmap,
	.poll_fd = mock_poll,
	.name = "mock-gc573",
};
