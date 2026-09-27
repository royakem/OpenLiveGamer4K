/* SPDX-License-Identifier: GPL-2.0-only */
#include "olg4k/backend.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/types.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/poll.h>
#include <time.h>
#include <unistd.h>

int olg4k_raw_ioctl(Olg4kDevice *dev, unsigned long request, void *arg)
{
	int e = 0;
	long ret = dev->backend->ioctl(dev->fd, request, arg, dev->impl, &e);

	if (ret < 0) {
		dev->last_errno = e;
		return -1;
	}
	return 0;
}

int olg4k_open(Olg4kDevice *dev, const Olg4kBackend *backend, void *impl,
	const char *path, Olg4kMode mode)
{
	Olg4kErr err = OLG4K_ERR_OK;
	int e = 0;

	memset(dev, 0, sizeof(*dev));
	dev->fd = -1;
	dev->backend = backend;
	dev->impl = impl;
	dev->mode = mode;

	if (backend->open(path, impl, &err, &e) < 0) {
		dev->last_errno = e;
		return -1;
	}
	dev->fd = e;
	return 0;
}

void olg4k_close(Olg4kDevice *dev)
{
	if (dev->fd >= 0 && dev->backend)
		dev->backend->close(dev->fd, dev->impl);
	dev->fd = -1;
	dev->streaming = false;
}

void olg4k_set_mode(Olg4kDevice *dev, Olg4kMode mode)
{
	dev->mode = mode;
}

const char *olg4k_err_string(Olg4kErr err)
{
	switch (err) {
		case OLG4K_ERR_OK: return "ok";
		case OLG4K_ERR_OPEN: return "open failed";
		case OLG4K_ERR_PERMISSION: return "permission denied";
		case OLG4K_ERR_NOT_FOUND: return "device not found";
		case OLG4K_ERR_BUSY: return "device busy";
		case OLG4K_ERR_MODE: return "not allowed in current mode";
		case OLG4K_ERR_TIMEOUT: return "timeout";
		default: return "backend error";
	}
}

static int map_errno(int e)
{
	switch (e) {
		case EACCES:
		case EPERM: return -1;
		default: return -1;
	}
}

int olg4k_querycap(Olg4kDevice *dev, struct v4l2_capability *cap)
{
	int ret = olg4k_raw_ioctl(dev, VIDIOC_QUERYCAP, cap);
	return ret ? map_errno(dev->last_errno) : 0;
}

int olg4k_enum_fmt(Olg4kDevice *dev, unsigned int index,
	struct v4l2_fmtdesc *fmt)
{
	memset(fmt, 0, sizeof(*fmt));
	fmt->index = index;
	fmt->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	int ret = olg4k_raw_ioctl(dev, VIDIOC_ENUM_FMT, fmt);
	return ret ? map_errno(dev->last_errno) : 0;
}

int olg4k_enum_framesize(Olg4kDevice *dev, __u32 fourcc, unsigned int index,
	struct v4l2_frmsizeenum *fsize)
{
	memset(fsize, 0, sizeof(*fsize));
	fsize->pixel_format = fourcc;
	fsize->index = index;
	int ret = olg4k_raw_ioctl(dev, VIDIOC_ENUM_FRAMESIZES, fsize);
	return ret ? map_errno(dev->last_errno) : 0;
}

int olg4k_enum_frameinterval(Olg4kDevice *dev, __u32 fourcc,
	unsigned int width, unsigned int height, unsigned int index,
	struct v4l2_frmivalenum *fival)
{
	memset(fival, 0, sizeof(*fival));
	fival->pixel_format = fourcc;
	fival->width = width;
	fival->height = height;
	fival->index = index;
	int ret = olg4k_raw_ioctl(dev, VIDIOC_ENUM_FRAMEINTERVALS, fival);
	return ret ? map_errno(dev->last_errno) : 0;
}

int olg4k_g_fmt(Olg4kDevice *dev, struct v4l2_format *f)
{
	f->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	int ret = olg4k_raw_ioctl(dev, VIDIOC_G_FMT, f);
	return ret ? map_errno(dev->last_errno) : 0;
}

int olg4k_g_parm(Olg4kDevice *dev, struct v4l2_streamparm *parm)
{
	memset(parm, 0, sizeof(*parm));
	parm->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	int ret = olg4k_raw_ioctl(dev, VIDIOC_G_PARM, parm);
	return ret ? map_errno(dev->last_errno) : 0;
}

int olg4k_enum_input(Olg4kDevice *dev, unsigned int index,
	struct v4l2_input *input)
{
	memset(input, 0, sizeof(*input));
	input->index = index;
	int ret = olg4k_raw_ioctl(dev, VIDIOC_ENUMINPUT, input);
	return ret ? map_errno(dev->last_errno) : 0;
}

int olg4k_try_fmt(Olg4kDevice *dev, struct v4l2_format *f)
{
	f->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	int ret = olg4k_raw_ioctl(dev, VIDIOC_TRY_FMT, f);
	return ret ? map_errno(dev->last_errno) : 0;
}

/* --- preview helpers: refuse while not in preview mode --- */

int olg4k_s_fmt(Olg4kDevice *dev, struct v4l2_format *f)
{
	if (dev->mode != OLG4K_MODE_PREVIEW) {
		dev->muting_ioctls++;
		dev->last_errno = EPERM;
		return -1;
	}
	f->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	int ret = olg4k_raw_ioctl(dev, VIDIOC_S_FMT, f);
	return ret ? map_errno(dev->last_errno) : 0;
}

int olg4k_s_parm(Olg4kDevice *dev, struct v4l2_streamparm *parm)
{
	if (dev->mode != OLG4K_MODE_PREVIEW) {
		dev->muting_ioctls++;
		dev->last_errno = EPERM;
		return -1;
	}
	parm->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	int ret = olg4k_raw_ioctl(dev, VIDIOC_S_PARM, parm);
	return ret ? map_errno(dev->last_errno) : 0;
}

int olg4k_reqbufs(Olg4kDevice *dev, struct v4l2_requestbuffers *rb)
{
	if (dev->mode != OLG4K_MODE_PREVIEW) {
		dev->muting_ioctls++;
		dev->last_errno = EPERM;
		return -1;
	}
	int ret = olg4k_raw_ioctl(dev, VIDIOC_REQBUFS, rb);
	return ret ? map_errno(dev->last_errno) : 0;
}

int olg4k_qbuf(Olg4kDevice *dev, struct v4l2_buffer *buf)
{
	if (dev->mode != OLG4K_MODE_PREVIEW) {
		dev->muting_ioctls++;
		dev->last_errno = EPERM;
		return -1;
	}
	int ret = olg4k_raw_ioctl(dev, VIDIOC_QBUF, buf);
	return ret ? map_errno(dev->last_errno) : 0;
}

int olg4k_dqbuf(Olg4kDevice *dev, struct v4l2_buffer *buf)
{
	if (dev->mode != OLG4K_MODE_PREVIEW) {
		dev->muting_ioctls++;
		dev->last_errno = EPERM;
		return -1;
	}
	int ret = olg4k_raw_ioctl(dev, VIDIOC_DQBUF, buf);
	return ret ? map_errno(dev->last_errno) : 0;
}

int olg4k_querybuf(Olg4kDevice *dev, struct v4l2_buffer *buf)
{
	if (dev->mode != OLG4K_MODE_PREVIEW) { dev->last_errno = EPERM; return -1; }
	buf->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	buf->memory = V4L2_MEMORY_MMAP;
	return olg4k_raw_ioctl(dev, VIDIOC_QUERYBUF, buf);
}

int olg4k_streamon(Olg4kDevice *dev, enum v4l2_buf_type type)
{
	if (dev->mode != OLG4K_MODE_PREVIEW) {
		dev->muting_ioctls++;
		dev->last_errno = EPERM;
		return -1;
	}
	int ret = olg4k_raw_ioctl(dev, VIDIOC_STREAMON, &type);
	if (!ret)
		dev->streaming = true;
	return ret ? map_errno(dev->last_errno) : 0;
}

int olg4k_streamoff(Olg4kDevice *dev, enum v4l2_buf_type type)
{
	if (dev->mode != OLG4K_MODE_PREVIEW) {
		dev->muting_ioctls++;
		dev->last_errno = EPERM;
		return -1;
	}
	int ret = olg4k_raw_ioctl(dev, VIDIOC_STREAMOFF, &type);
	dev->streaming = false;
	return ret ? map_errno(dev->last_errno) : 0;
}

int olg4k_mmap(Olg4kDevice *dev, void **addr, size_t length, int prot,
	off_t offset)
{
	if (dev->mode != OLG4K_MODE_PREVIEW) {
		dev->muting_ioctls++;
		dev->last_errno = EPERM;
		return -1;
	}
	Olg4kErr err;
	int e = 0;
	if (dev->backend->mmap(dev->fd, addr, length, prot, MAP_SHARED,
		offset, dev->impl, &err, &e) < 0) {
		dev->last_errno = e;
		return -1;
	}
	return 0;
}

int olg4k_munmap(Olg4kDevice *dev, void *addr, size_t length)
{
	if (dev->backend->munmap)
		return dev->backend->munmap(addr, length, dev->impl);
	return munmap(addr, length);
}
