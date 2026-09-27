/* SPDX-License-Identifier: GPL-2.0-only */
/* V4L2 MMAP preview worker. */
#include "olg4k/worker.h"
#include "olg4k/convert.h"

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

struct Olg4kMappedBuffer {
	void *address;
	size_t length;
};

struct Olg4kWorker {
	Olg4kDevice *dev;
	_Atomic int state;
	_Atomic bool stop_requested;
	_Atomic unsigned long long frames;
	pthread_t thread;
	bool have_thread;
	bool buffers_requested;
	bool streaming;
	unsigned int nbufs;
	unsigned int width;
	unsigned int height;
	unsigned int fourcc;
	unsigned int stride;
	uint32_t ycbcr_enc;
	uint32_t quantization;
	size_t rgb_size;
	struct Olg4kMappedBuffer maps[16];
	uint8_t *frame;
	pthread_mutex_t slot_lock;
	Olg4kFrameCallback callback;
	void *user_data;
	char error[256];
};

static void cleanup(struct Olg4kWorker *worker)
{
	if (worker->streaming) {
		(void)olg4k_streamoff(worker->dev, V4L2_BUF_TYPE_VIDEO_CAPTURE);
		worker->streaming = false;
	}
	for (unsigned int i = 0; i < worker->nbufs; i++) {
		if (worker->maps[i].address) {
			(void)olg4k_munmap(worker->dev, worker->maps[i].address,
					   worker->maps[i].length);
			worker->maps[i].address = NULL;
		}
	}
	if (worker->buffers_requested) {
		struct v4l2_requestbuffers request = {
			.type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
			.memory = V4L2_MEMORY_MMAP,
		};
		(void)olg4k_reqbufs(worker->dev, &request);
		worker->buffers_requested = false;
	}
}

static void set_error(struct Olg4kWorker *worker, const char *operation,
		      int error_number)
{
	snprintf(worker->error, sizeof(worker->error), "%s failed: %s",
		 operation, strerror(error_number));
	atomic_store(&worker->state, OLG4K_WORKER_ERROR);
}

static void *worker_main(void *argument)
{
	struct Olg4kWorker *worker = argument;

	while (!atomic_load(&worker->stop_requested)) {
		struct pollfd pollfd = {
			.fd = worker->dev->fd,
			.events = POLLIN | POLLPRI,
		};
		int ready;

		if (worker->dev->backend->poll_fd)
			ready = worker->dev->backend->poll_fd(worker->dev->fd,
				worker->dev->impl, &pollfd, 1, 50);
		else
			ready = poll(&pollfd, 1, 50);
		if (ready < 0) {
			if (errno == EINTR)
				continue;
			set_error(worker, "poll", errno);
			break;
		}
		if (ready == 0)
			continue;

		struct v4l2_buffer buffer = {
			.type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
			.memory = V4L2_MEMORY_MMAP,
		};
		if (olg4k_dqbuf(worker->dev, &buffer)) {
			int error_number = worker->dev->last_errno;
			if (error_number == EAGAIN || error_number == EWOULDBLOCK)
				continue;
			set_error(worker, "DQBUF", error_number);
			break;
		}
		if (buffer.index >= worker->nbufs || !worker->maps[buffer.index].address ||
		    buffer.bytesused == 0 ||
		    buffer.bytesused > worker->maps[buffer.index].length) {
			set_error(worker, "invalid dequeued buffer", EINVAL);
			break;
		}

		pthread_mutex_lock(&worker->slot_lock);
		bool converted = olg4k_frame_to_rgb24_extent(
			worker->fourcc, worker->width, worker->height, worker->stride,
			buffer.bytesused, worker->maps[buffer.index].address,
			worker->frame, worker->rgb_size, worker->ycbcr_enc,
			worker->quantization);
		if (converted) {
			atomic_fetch_add(&worker->frames, 1);
			worker->callback(worker, worker->frame, worker->width,
					 worker->height, worker->user_data);
		}
		pthread_mutex_unlock(&worker->slot_lock);
		if (!converted) {
			set_error(worker, "frame conversion", EINVAL);
			break;
		}

		buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		buffer.memory = V4L2_MEMORY_MMAP;
		if (olg4k_qbuf(worker->dev, &buffer)) {
			set_error(worker, "QBUF", worker->dev->last_errno);
			break;
		}
	}
	if (atomic_load(&worker->state) == OLG4K_WORKER_ERROR)
		cleanup(worker);
	return NULL;
}

bool olg4k_worker_prepare(struct Olg4kWorker **out, Olg4kDevice *dev,
			  unsigned int nbufs, Olg4kFrameCallback callback,
			  void *user_data)
{
	if (!out || !dev || !callback || nbufs < 1 || nbufs > 16 ||
	    dev->mode != OLG4K_MODE_PREVIEW)
		return false;
	*out = NULL;

	struct Olg4kWorker *worker = calloc(1, sizeof(*worker));
	if (!worker)
		return false;
	worker->dev = dev;
	worker->nbufs = nbufs;
	worker->callback = callback;
	worker->user_data = user_data;
	pthread_mutex_init(&worker->slot_lock, NULL);
	atomic_init(&worker->state, OLG4K_WORKER_IDLE);
	atomic_init(&worker->stop_requested, false);
	atomic_init(&worker->frames, 0);

	struct v4l2_format format = {0};
	if (olg4k_g_fmt(dev, &format))
		goto error;
	const struct v4l2_pix_format *pix = &format.fmt.pix;
	worker->width = pix->width;
	worker->height = pix->height;
	worker->fourcc = pix->pixelformat;
	worker->stride = pix->bytesperline;
	if (!worker->width || !worker->height ||
	    (size_t)worker->width > SIZE_MAX / worker->height / 3)
		goto error;
	worker->rgb_size = (size_t)worker->width * worker->height * 3;
	worker->ycbcr_enc = pix->ycbcr_enc;
	if (worker->ycbcr_enc == V4L2_YCBCR_ENC_DEFAULT)
		worker->ycbcr_enc = V4L2_MAP_YCBCR_ENC_DEFAULT(pix->colorspace);
	worker->quantization = pix->quantization;
	if (worker->quantization == V4L2_QUANTIZATION_DEFAULT)
		worker->quantization = V4L2_MAP_QUANTIZATION_DEFAULT(
			worker->fourcc == V4L2_PIX_FMT_RGB24 ||
			worker->fourcc == V4L2_PIX_FMT_BGR24,
			pix->colorspace, worker->ycbcr_enc);
	worker->frame = malloc(worker->rgb_size);
	if (!worker->frame)
		goto error;

	struct v4l2_requestbuffers request = {
		.count = nbufs,
		.type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
		.memory = V4L2_MEMORY_MMAP,
	};
	if (olg4k_reqbufs(dev, &request))
		goto error;
	worker->buffers_requested = true;
	if (!request.count || request.count > nbufs)
		goto error;
	worker->nbufs = request.count;

	for (unsigned int i = 0; i < worker->nbufs; i++) {
		struct v4l2_buffer buffer = {
			.index = i,
			.type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
			.memory = V4L2_MEMORY_MMAP,
		};
		if (olg4k_querybuf(dev, &buffer) || !buffer.length)
			goto error;
		worker->maps[i].length = buffer.length;
		if (olg4k_mmap(dev, &worker->maps[i].address, buffer.length,
			       PROT_READ | PROT_WRITE, buffer.m.offset))
			goto error;
	}
	*out = worker;
	return true;

error:
	cleanup(worker);
	free(worker->frame);
	pthread_mutex_destroy(&worker->slot_lock);
	free(worker);
	return false;
}

bool olg4k_worker_start(struct Olg4kWorker *worker)
{
	if (!worker || atomic_load(&worker->state) != OLG4K_WORKER_IDLE ||
	    !worker->buffers_requested)
		return false;
	for (unsigned int i = 0; i < worker->nbufs; i++) {
		if (!worker->maps[i].address)
			return false;
		struct v4l2_buffer buffer = {
			.index = i,
			.type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
			.memory = V4L2_MEMORY_MMAP,
		};
		if (olg4k_qbuf(worker->dev, &buffer))
			goto error;
	}
	if (olg4k_streamon(worker->dev, V4L2_BUF_TYPE_VIDEO_CAPTURE))
		goto error;
	worker->streaming = true;
	atomic_store(&worker->stop_requested, false);
	atomic_store(&worker->state, OLG4K_WORKER_RUNNING);
	if (pthread_create(&worker->thread, NULL, worker_main, worker) != 0) {
		atomic_store(&worker->state, OLG4K_WORKER_IDLE);
		goto error;
	}
	worker->have_thread = true;
	return true;

error:
	cleanup(worker);
	return false;
}

void olg4k_worker_stop(struct Olg4kWorker *worker)
{
	if (!worker)
		return;
	atomic_store(&worker->stop_requested, true);
	if (worker->have_thread) {
		(void)pthread_join(worker->thread, NULL);
		worker->have_thread = false;
	}
	cleanup(worker);
	if (atomic_load(&worker->state) != OLG4K_WORKER_ERROR)
		atomic_store(&worker->state, OLG4K_WORKER_IDLE);
}

void olg4k_worker_free(struct Olg4kWorker *worker)
{
	if (!worker)
		return;
	olg4k_worker_stop(worker);
	free(worker->frame);
	pthread_mutex_destroy(&worker->slot_lock);
	free(worker);
}

Olg4kWorkerState olg4k_worker_state(const struct Olg4kWorker *worker)
{
	return worker ? (Olg4kWorkerState)atomic_load(&worker->state) :
		OLG4K_WORKER_IDLE;
}

const char *olg4k_worker_error(const struct Olg4kWorker *worker)
{
	return worker && worker->error[0] ? worker->error : "";
}

unsigned long long olg4k_worker_frames(const struct Olg4kWorker *worker)
{
	return worker ? atomic_load(&worker->frames) : 0;
}
