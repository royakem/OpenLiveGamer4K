// SPDX-License-Identifier: GPL-2.0-only
/*
 * Standalone stereo HDMI capture PCM frontend. The transport is supplied by
 * the caller; this file does not program or assume any audio hardware.
 */
#include <linux/atomic.h>
#include <linux/errno.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/overflow.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/workqueue.h>

#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>

#include "gc573_audio.h"

#define GC573_AUDIO_RATE		48000U
#define GC573_AUDIO_CHANNELS		2U
#define GC573_AUDIO_SAMPLE_BYTES	2U
#define GC573_AUDIO_FRAME_BYTES		(GC573_AUDIO_CHANNELS * \
						 GC573_AUDIO_SAMPLE_BYTES)
#define GC573_AUDIO_PERIOD_FRAMES	960U
#define GC573_AUDIO_PERIOD_BYTES	(GC573_AUDIO_PERIOD_FRAMES * \
						 GC573_AUDIO_FRAME_BYTES)
#define GC573_AUDIO_PERIODS_MIN		2U
#define GC573_AUDIO_PERIODS_MAX		16U
#define GC573_AUDIO_BUFFER_BYTES_MAX	(GC573_AUDIO_PERIOD_BYTES * \
						 GC573_AUDIO_PERIODS_MAX)

static const struct snd_pcm_hardware gc573_audio_hardware = {
	.info = SNDRV_PCM_INFO_MMAP |
		SNDRV_PCM_INFO_MMAP_VALID |
		SNDRV_PCM_INFO_INTERLEAVED |
		SNDRV_PCM_INFO_BLOCK_TRANSFER,
	.formats = SNDRV_PCM_FMTBIT_S16_LE,
	.rates = SNDRV_PCM_RATE_48000,
	.rate_min = GC573_AUDIO_RATE,
	.rate_max = GC573_AUDIO_RATE,
	.channels_min = GC573_AUDIO_CHANNELS,
	.channels_max = GC573_AUDIO_CHANNELS,
	.buffer_bytes_max = GC573_AUDIO_BUFFER_BYTES_MAX,
	.period_bytes_min = GC573_AUDIO_PERIOD_BYTES,
	.period_bytes_max = GC573_AUDIO_PERIOD_BYTES,
	.periods_min = GC573_AUDIO_PERIODS_MIN,
	.periods_max = GC573_AUDIO_PERIODS_MAX,
};

struct gc573_audio {
	struct snd_card *card;
	struct snd_pcm_substream *substream;
	struct gc573_audio_ops ops;
	void *context;
	struct mutex ring_lock;
	struct work_struct period_work;
	struct work_struct xrun_work;
	atomic64_t hw_total;
	u64 notified_total;
	atomic_t running;
	bool allocated;
	bool registered;
	bool opened;
	bool prepared;
};

static struct gc573_audio *gc573_audio_from_substream(
		struct snd_pcm_substream *substream)
{
	return substream->pcm->private_data;
}

static int gc573_audio_open(struct snd_pcm_substream *substream)
{
	struct gc573_audio *audio = gc573_audio_from_substream(substream);
	struct snd_pcm_runtime *runtime = substream->runtime;
	int ret;

	if (!audio || !READ_ONCE(audio->registered))
		return -ENODEV;
	if (audio->opened)
		return -EBUSY;

	runtime->hw = gc573_audio_hardware;
	ret = snd_pcm_hw_constraint_integer(runtime, SNDRV_PCM_HW_PARAM_PERIODS);
	if (ret < 0)
		return ret;

	WRITE_ONCE(audio->substream, substream);
	audio->opened = true;
	return 0;
}

static int gc573_audio_close(struct snd_pcm_substream *substream)
{
	struct gc573_audio *audio = gc573_audio_from_substream(substream);

	if (atomic_xchg(&audio->running, 0))
		audio->ops.stop(audio->context);
	WRITE_ONCE(audio->substream, NULL);
	audio->prepared = false;
	audio->opened = false;
	return 0;
}

static int gc573_audio_hw_params(struct snd_pcm_substream *substream,
				 struct snd_pcm_hw_params *params)
{
	return snd_pcm_lib_malloc_pages(substream,
					params_buffer_bytes(params));
}

static int gc573_audio_hw_free(struct snd_pcm_substream *substream)
{
	struct gc573_audio *audio = gc573_audio_from_substream(substream);

	audio->prepared = false;
	return snd_pcm_lib_free_pages(substream);
}

static int gc573_audio_prepare(struct snd_pcm_substream *substream)
{
	struct gc573_audio *audio = gc573_audio_from_substream(substream);
	struct snd_pcm_runtime *runtime = substream->runtime;

	if (atomic_read(&audio->running))
		return -EBUSY;
	if (!runtime->dma_area || !runtime->buffer_size ||
	    runtime->period_size != GC573_AUDIO_PERIOD_FRAMES)
		return -EINVAL;

	mutex_lock(&audio->ring_lock);
	atomic64_set(&audio->hw_total, 0);
	audio->notified_total = 0;
	audio->prepared = true;
	mutex_unlock(&audio->ring_lock);
	return 0;
}

static int gc573_audio_trigger(struct snd_pcm_substream *substream, int cmd)
{
	struct gc573_audio *audio = gc573_audio_from_substream(substream);
	int ret;

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		if (!audio->prepared)
			return -EINVAL;
		if (atomic_xchg(&audio->running, 1))
			return 0;
		ret = audio->ops.start(audio->context);
		if (ret) {
			atomic_set(&audio->running, 0);
			/* start() must unwind and leave its transport quiescent. */
			return ret;
		}
		return 0;
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		if (atomic_xchg(&audio->running, 0))
			audio->ops.stop(audio->context);
		return 0;
	default:
		return -EINVAL;
	}
}

/*
 * This is intentionally separate from .trigger(STOP). ALSA calls sync_stop
 * after dropping its stream lock, so joining the transport worker here cannot
 * deadlock against that worker entering snd_pcm_period_elapsed().
 */
static int gc573_audio_sync_stop(struct snd_pcm_substream *substream)
{
	struct gc573_audio *audio = gc573_audio_from_substream(substream);

	audio->ops.sync_stop(audio->context);
	if (current_work() != &audio->period_work)
		cancel_work_sync(&audio->period_work);
	if (current_work() != &audio->xrun_work)
		cancel_work_sync(&audio->xrun_work);
	return 0;
}

static snd_pcm_uframes_t gc573_audio_pointer(
		struct snd_pcm_substream *substream)
{
	struct gc573_audio *audio = gc573_audio_from_substream(substream);
	struct snd_pcm_runtime *runtime = substream->runtime;
	u64 total;

	if (!runtime->buffer_size)
		return 0;
	total = atomic64_read(&audio->hw_total);
	return total % runtime->buffer_size;
}

static const struct snd_pcm_ops gc573_audio_pcm_ops = {
	.open = gc573_audio_open,
	.close = gc573_audio_close,
	.ioctl = snd_pcm_lib_ioctl,
	.hw_params = gc573_audio_hw_params,
	.hw_free = gc573_audio_hw_free,
	.prepare = gc573_audio_prepare,
	.trigger = gc573_audio_trigger,
	.sync_stop = gc573_audio_sync_stop,
	.pointer = gc573_audio_pointer,
	.mmap = snd_pcm_lib_default_mmap,
};

static void gc573_audio_period_work(struct work_struct *work)
{
	struct gc573_audio *audio = container_of(work, struct gc573_audio,
							 period_work);
	struct snd_pcm_substream *substream;
	struct snd_pcm_runtime *runtime;

	for (;;) {
		u64 hw_total;
		bool period_ready;

		if (!atomic_read(&audio->running))
			return;
		substream = READ_ONCE(audio->substream);
		if (!substream)
			return;
		runtime = READ_ONCE(substream->runtime);
		if (!runtime || !runtime->period_size)
			return;

		mutex_lock(&audio->ring_lock);
		hw_total = atomic64_read(&audio->hw_total);
		period_ready = hw_total >= audio->notified_total &&
			hw_total - audio->notified_total >= runtime->period_size;
		if (period_ready)
			audio->notified_total += runtime->period_size;
		mutex_unlock(&audio->ring_lock);
		if (!period_ready)
			return;

		/* May cause XRUN handling; never hold ring_lock across this call. */
		snd_pcm_period_elapsed(substream);
	}
}

static void gc573_audio_xrun_work(struct work_struct *work)
{
	struct gc573_audio *audio = container_of(work, struct gc573_audio,
							 xrun_work);
	struct snd_pcm_substream *substream;

	if (!atomic_read(&audio->running))
		return;
	substream = READ_ONCE(audio->substream);
	if (substream)
		snd_pcm_stop_xrun(substream);
}

int gc573_audio_alloc(struct device *parent,
		      const struct gc573_audio_ops *ops, void *context,
		      struct gc573_audio **audio_out)
{
	struct snd_pcm *pcm;
	struct snd_card *card;
	struct gc573_audio *audio;
	int ret;

	if (!parent || !ops || !ops->start || !ops->stop || !ops->sync_stop ||
	    !audio_out)
		return -EINVAL;
	*audio_out = NULL;

	ret = snd_card_new(parent, -1, NULL, THIS_MODULE, sizeof(*audio), &card);
	if (ret)
		return ret;
	audio = card->private_data;
	audio->card = card;
	audio->ops = *ops;
	audio->context = context;
	mutex_init(&audio->ring_lock);
	atomic64_set(&audio->hw_total, 0);
	atomic_set(&audio->running, 0);
	INIT_WORK(&audio->period_work, gc573_audio_period_work);
	INIT_WORK(&audio->xrun_work, gc573_audio_xrun_work);

	strscpy(card->driver, "gc573-audio", sizeof(card->driver));
	strscpy(card->shortname, "OpenLiveGamer4K HDMI Audio",
		sizeof(card->shortname));
	snprintf(card->longname, sizeof(card->longname),
		 "OpenLiveGamer4K HDMI audio capture (%s)", dev_name(parent));

	ret = snd_pcm_new(card, "HDMI Capture", 0, 0, 1, &pcm);
	if (ret)
		goto err_card;
	pcm->private_data = audio;
	pcm->nonatomic = true;
	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_CAPTURE, &gc573_audio_pcm_ops);
	snd_pcm_lib_preallocate_pages_for_all(pcm, SNDRV_DMA_TYPE_VMALLOC,
						     NULL, 0,
						     GC573_AUDIO_BUFFER_BYTES_MAX);

	audio->allocated = true;
	*audio_out = audio;
	return 0;

err_card:
	snd_card_free(card);
	return ret;
}

int gc573_audio_register(struct gc573_audio *audio)
{
	int ret;

	if (!audio || !audio->allocated)
		return -EINVAL;
	if (audio->registered)
		return -EALREADY;

	/* Publish state first; callbacks can run as soon as registration starts. */
	WRITE_ONCE(audio->registered, true);
	ret = snd_card_register(audio->card);
	if (ret)
		WRITE_ONCE(audio->registered, false);
	return ret;
}

void gc573_audio_cleanup(struct gc573_audio *audio)
{
	if (!audio || !audio->allocated)
		return;

	WRITE_ONCE(audio->registered, false);
	snd_card_disconnect(audio->card);
	if (atomic_xchg(&audio->running, 0))
		audio->ops.stop(audio->context);
	audio->ops.sync_stop(audio->context);
	if (current_work() != &audio->period_work)
		cancel_work_sync(&audio->period_work);
	if (current_work() != &audio->xrun_work)
		cancel_work_sync(&audio->xrun_work);
	audio->allocated = false;
	snd_card_free(audio->card);
}

int gc573_audio_push(struct gc573_audio *audio, const void *data, size_t bytes)
{
	struct snd_pcm_substream *substream;
	struct snd_pcm_runtime *runtime;
	snd_pcm_uframes_t appl_ptr;
	snd_pcm_uframes_t buffer_frames;
	snd_pcm_uframes_t hw_mod;
	snd_pcm_uframes_t used_frames;
	snd_pcm_uframes_t write_pos;
	snd_pcm_uframes_t first_frames;
	u64 hw_total;
	u64 next_total;
	u64 appl_mod;
	size_t frames;
	bool overrun = false;
	int ret = 0;

	if (!audio || !data || !bytes || bytes % GC573_AUDIO_FRAME_BYTES)
		return -EINVAL;
	if (!READ_ONCE(audio->registered) || !atomic_read(&audio->running))
		return -ENODEV;
	frames = bytes / GC573_AUDIO_FRAME_BYTES;
	substream = READ_ONCE(audio->substream);
	if (!substream)
		return -ENODEV;
	runtime = READ_ONCE(substream->runtime);
	if (!runtime || !runtime->dma_area || !runtime->control ||
	    !runtime->buffer_size || !runtime->boundary)
		return -ENODEV;

	mutex_lock(&audio->ring_lock);
	if (!atomic_read(&audio->running) ||
	    substream != READ_ONCE(audio->substream)) {
		ret = -ENODEV;
		goto out_unlock;
	}
	buffer_frames = runtime->buffer_size;
	hw_total = atomic64_read(&audio->hw_total);
	if (check_add_overflow(hw_total, (u64)frames, &next_total)) {
		overrun = true;
		goto out_unlock;
	}

	/* appl_ptr and the driver pointer share ALSA's wrapping boundary. */
	appl_ptr = READ_ONCE(runtime->control->appl_ptr);
	if (appl_ptr >= runtime->boundary) {
		ret = -EINVAL;
		goto out_unlock;
	}
	appl_mod = hw_total % runtime->boundary;
	hw_mod = appl_mod;
	if (hw_mod >= appl_ptr)
		used_frames = hw_mod - appl_ptr;
	else
		used_frames = runtime->boundary - (appl_ptr - hw_mod);
	if (used_frames > buffer_frames || frames > buffer_frames - used_frames) {
		overrun = true;
		goto out_unlock;
	}

	write_pos = hw_total % buffer_frames;
	first_frames = min_t(snd_pcm_uframes_t, frames,
			     buffer_frames - write_pos);
	memcpy(runtime->dma_area + write_pos * GC573_AUDIO_FRAME_BYTES,
	       data, first_frames * GC573_AUDIO_FRAME_BYTES);
	if (frames > first_frames)
		memcpy(runtime->dma_area,
		       (const u8 *)data + first_frames * GC573_AUDIO_FRAME_BYTES,
	       (frames - first_frames) * GC573_AUDIO_FRAME_BYTES);
	atomic64_set(&audio->hw_total, next_total);
	ret = 0;

out_unlock:
	mutex_unlock(&audio->ring_lock);
	if (overrun) {
		schedule_work(&audio->xrun_work);
		return -EPIPE;
	}
	if (!ret)
		schedule_work(&audio->period_work);
	return ret;
}

/* Transport calls this from its worker; ALSA stop runs on separate work. */
void gc573_audio_xrun(struct gc573_audio *audio)
{
	if (audio && atomic_read(&audio->running))
		schedule_work(&audio->xrun_work);
}
