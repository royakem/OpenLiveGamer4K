/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef OLG4K_WORKER_H
#define OLG4K_WORKER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "olg4k/backend.h"

struct Olg4kWorker;

/* Called on the capture worker thread with a borrowed RGB24 frame.
 * Copy before returning if delivering it asynchronously to a UI. */
typedef void (*Olg4kFrameCallback)(struct Olg4kWorker *worker,
	const uint8_t *rgb24, unsigned int width, unsigned int height,
	void *user_data);

typedef enum {
	OLG4K_WORKER_IDLE,
	OLG4K_WORKER_RUNNING,
	OLG4K_WORKER_ERROR,
} Olg4kWorkerState;

/* Configure a worker: device already in OLG4K_MODE_PREVIEW with the
 * desired format/rate applied. Buffers are requested and mmap'ed here. */
bool olg4k_worker_prepare(struct Olg4kWorker **out, Olg4kDevice *dev,
	unsigned int nbufs, Olg4kFrameCallback cb, void *user_data);

/* Start the DQBUF thread. Returns false if the stream could not start. */
bool olg4k_worker_start(struct Olg4kWorker *w);

/* Request a stop; blocks until the thread has released the device. */
void olg4k_worker_stop(struct Olg4kWorker *w);

void olg4k_worker_free(struct Olg4kWorker *w);
Olg4kWorkerState olg4k_worker_state(const struct Olg4kWorker *w);
const char *olg4k_worker_error(const struct Olg4kWorker *w);
unsigned long long olg4k_worker_frames(const struct Olg4kWorker *w);

#endif
