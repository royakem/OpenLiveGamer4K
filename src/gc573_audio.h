/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef GC573_AUDIO_H
#define GC573_AUDIO_H

#include <linux/device.h>
#include <linux/types.h>

struct gc573_audio;

/*
 * Transport callbacks run from ALSA PCM process context. start() may sleep.
 * stop() is called by .trigger(STOP): it must only prevent further DMA/work
 * scheduling and must not wait for a worker. sync_stop() runs from ALSA's
 * .sync_stop hook, after the stream lock is released, and must drain every
 * transport worker that can call gc573_audio_push().
 */
struct gc573_audio_ops {
	int (*start)(void *context);
	void (*stop)(void *context);
	void (*sync_stop)(void *context);
};

/* Allocate the PCM/card without publishing it to userspace. */
int gc573_audio_alloc(struct device *parent,
		      const struct gc573_audio_ops *ops, void *context,
		      struct gc573_audio **audio);

/* Publish a previously allocated capture PCM card. */
int gc573_audio_register(struct gc573_audio *audio);

/* Disconnect, drain callbacks/work, and release the PCM/card allocation. */
void gc573_audio_cleanup(struct gc573_audio *audio);

/*
 * Push complete interleaved stereo S16_LE frames from deferred process
 * context. Pass the frontend object directly. Returns 0 on success, -EINVAL
 * for malformed input, -ENODEV when no active PCM accepts
 * data, and -EPIPE after scheduling an ALSA XRUN on ring overrun.
 */
int gc573_audio_push(struct gc573_audio *audio, const void *data, size_t bytes);

void gc573_audio_xrun(struct gc573_audio *audio);

#endif /* GC573_AUDIO_H */
