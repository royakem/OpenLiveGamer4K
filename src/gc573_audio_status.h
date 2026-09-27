/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef GC573_AUDIO_STATUS_H
#define GC573_AUDIO_STATUS_H

#include <linux/types.h>

/*
 * Serialized IT6805 audio diagnostic. N/CTS are decoded from one measurement
 * latched using the reference driver's bank-0 0x86 bit 0 control. The latch
 * bit is restored after sampling; the receiver bank is restored to bank 0.
 * No sample rate is inferred here.
 */
struct gc573_device;

struct gc573_audio_status {
	u8 infoframe_b0;
	u8 infoframe_b1;
	u8 infoframe_b2;
	u8 audio_rate_b5;
	u8 audio_rate_b6;
	u8 receiver_scdt_19;
	/* Raw bank-0 audio controls, not interrupt status or measured clocks. */
	u8 audio_control_81;
	u8 audio_control_8a;
	u8 audio_control_8c;
	/* Raw bank-1 C7 output pin tristate control. */
	u8 audio_output_c7;
	/* Bank 2 N high/mid/low and CTS low/mid/high register bytes. */
	u8 n_high;
	u8 n_mid;
	u8 n_low;
	u8 cts_low;
	u8 cts_mid;
	u8 cts_high;
	u32 n_decoded;
	u32 cts_decoded;
	/* Encoded IT6805 receiver rate, not a measured sample rate. */
	u8 receiver_rate_code;
	bool infoframe_valid;
	bool video_scdt;
	bool counters_coherent;
	/* Valid LPCM, two-channel allocation, and the IT6805 48 kHz rate code. */
	bool receiver_reports_48k_lpcm_stereo;
};

/*
 * Caller holds dev->vdev->lock to serialize receiver bank access; this
 * function also acquires dev->lock internally. Do not call without a
 * registered vdev or while already holding dev->lock.
 */
int gc573_receiver_audio_snapshot(struct gc573_device *dev,
				  struct gc573_audio_status *status);

/* Caller holds dev->vdev->lock; explicitly untristates IT6805 audio pins. */
int gc573_receiver_untristate_audio(struct gc573_device *dev);

/*
 * Experimental format refresh, audio-logic reset and pin enable. Caller holds
 * dev->vdev->lock throughout; takes dev->lock internally (do not already hold
 * it). Requires SCDT and receiver-reported 48 kHz LPCM stereo. Returns -ENOLINK
 * without SCDT, -EOPNOTSUPP for other formats, or an I2C error. Attempts bank-0
 * restoration after every banked transaction, including errors.
 * Does not measure N/CTS/TMDS, force a rate, or prove PCM output is active.
 */
int gc573_receiver_prepare_audio(struct gc573_device *dev);

#endif /* GC573_AUDIO_STATUS_H */
