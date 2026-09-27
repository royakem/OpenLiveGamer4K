/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef GC573_AUDIO_CLOCK_MATH_H
#define GC573_AUDIO_CLOCK_MATH_H

/*
 * Pure math for the experimental IT6805 audio clock path. Kept free of
 * hardware I/O (no i2c, no bank switching, no sleeps) so the same code the
 * module executes can be unit-tested in userspace (see
 * tests/test_audio_clock_math.c). Provenance:
 *   livegamer4k/internal/gc555-reference/gc555-it6805-core.c
 *   (it6805_audio_rate_code, it6805_measure_audio_tmds_locked,
 *    it6805_update_audio_clock_locked) and the shipped GC573 vendor object
 *   AverMediaLib_64.a:ite6805_sys.o, iTE6805_Enable_Audio_Output (0x499e).
 *
 * Both the kernel module and the userspace test compile this header. All
 * types and macros used here (u8/u16/u32/u64, GC573_ARRAY_SIZE, GC573_BIT)
 * are defined by the includer, so the header stays self-contained.
 */

#ifndef GC573_ARRAY_SIZE
#define GC573_ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#endif
#ifndef GC573_BIT
#define GC573_BIT(n) (1u << (n))
#endif

/* IT6805 audio rate-code values (reference it6805_audio_rate_code enum). */
#define GC573_IT6805_AUDIO_RATE_44_1_KHZ  0x00
#define GC573_IT6805_AUDIO_RATE_48_KHZ    0x02
#define GC573_IT6805_AUDIO_RATE_32_KHZ    0x03
#define GC573_IT6805_AUDIO_RATE_384_KHZ   0x05
#define GC573_IT6805_AUDIO_RATE_88_2_KHZ  0x08
#define GC573_IT6805_AUDIO_RATE_768_KHZ   0x09
#define GC573_IT6805_AUDIO_RATE_96_KHZ    0x0a
#define GC573_IT6805_AUDIO_RATE_64_KHZ    0x0b
#define GC573_IT6805_AUDIO_RATE_176_4_KHZ 0x0c
#define GC573_IT6805_AUDIO_RATE_192_KHZ   0x0e
#define GC573_IT6805_AUDIO_RATE_256_KHZ   0x1b
#define GC573_IT6805_AUDIO_RATE_128_KHZ   0x2b
#define GC573_IT6805_AUDIO_RATE_1024_KHZ  0x35
#define GC573_IT6805_AUDIO_RATE_512_KHZ   0x3b

/* Audio TMDS clock sampling (reference it6805_measure_audio_tmds). */
#define GC573_IT6805_AUDIO_TMDS_SAMPLES   10
#define GC573_IT6805_AUDIO_TMDS_DELAY_MS  3
/* Rate-override stability gate: force 0x81[6]/0x8a[5:0] only after this many
 * consecutive confirmed mismatches (reference IT6805_AUDIO_RATE_MISMATCH_LIMIT;
 * vendor iTE6805_Enable_Audio_Output forces at counter 0x10, i.e. 16). */
#define GC573_IT6805_AUDIO_RATE_MISMATCH_LIMIT 16

/*
 * Reference it6805_audio_rate_code: bin a sample rate in kHz into an IT6805
 * rate code. Out-of-range values map to the 1024 kHz code (reference behavior).
 */
static inline u8 gc573_it6805_audio_rate_code(u32 rate_khz)
{
	static const struct {
		u16 min_khz;
		u16 max_khz;
		u8 code;
	} ranges[] = {
		{ 26, 38, GC573_IT6805_AUDIO_RATE_32_KHZ },
		{ 39, 45, GC573_IT6805_AUDIO_RATE_44_1_KHZ },
		{ 46, 58, GC573_IT6805_AUDIO_RATE_48_KHZ },
		{ 59, 78, GC573_IT6805_AUDIO_RATE_64_KHZ },
		{ 79, 92, GC573_IT6805_AUDIO_RATE_88_2_KHZ },
		{ 93, 106, GC573_IT6805_AUDIO_RATE_96_KHZ },
		{ 107, 166, GC573_IT6805_AUDIO_RATE_128_KHZ },
		{ 167, 182, GC573_IT6805_AUDIO_RATE_176_4_KHZ },
		{ 183, 202, GC573_IT6805_AUDIO_RATE_192_KHZ },
		{ 225, 320, GC573_IT6805_AUDIO_RATE_256_KHZ },
		{ 321, 448, GC573_IT6805_AUDIO_RATE_384_KHZ },
		{ 449, 638, GC573_IT6805_AUDIO_RATE_512_KHZ },
		{ 639, 894, GC573_IT6805_AUDIO_RATE_768_KHZ },
	};
	size_t i;

	for (i = 0; i < GC573_ARRAY_SIZE(ranges); i++) {
		if (rate_khz >= ranges[i].min_khz &&
		    rate_khz <= ranges[i].max_khz)
			return ranges[i].code;
	}
	return GC573_IT6805_AUDIO_RATE_1024_KHZ;
}

/*
 * Receiver internal rate-code decode. Vendor functional path
 * (iTE6805_Enable_Audio_Output at 0x4d39-0x4d71) and reference
 * it6805_update_audio_clock_locked both use B6 for the upper bits and B5 for
 * the lower four: (B6 >> 2 & 0x30) | (B5 & 0x0f). This is distinct from the
 * diagnostic readout (iTE6805_Show_AUD_Info), which used B5 for both halves.
 */
static inline u8 gc573_it6805_decode_receiver_rate(u8 b5, u8 b6)
{
	return ((b6 >> 2) & 0x30) | (b5 & 0x0f);
}

/*
 * Reference it6805_update_audio_clock_locked:
 *   rate_khz = tmds_clock_khz * N / (CTS << 7)
 * N and CTS come from the latched 0xbe-0xc2 registers.
 */
static inline u32 gc573_it6805_compute_rate_khz(u32 tmds_khz, u32 n, u32 cts)
{
	if (!cts)
		return 0;
	return (u32)(((u64)tmds_khz * n) / ((u64)cts << 7));
}

/*
 * Reference it6805_measure_audio_tmds_locked: reconstruct the audio TMDS
 * (pixel) clock in kHz from the stored OCLK reference, the per-sample counter
 * factor in 0x43, and the summed 10-sample edge counts.
 *   clock_khz = reference_khz * SAMPLES * factor / sample_sum
 * where factor is 0x400/0x200/0x100/0x080 for 0x43 bits 7/6/5/none.
 */
static inline u32 gc573_it6805_tmds_clock_khz(u32 reference_khz, u8 reg43,
					      u32 sample_sum)
{
	u32 factor;

	if (!reference_khz || !sample_sum)
		return 0;

	if (reg43 & GC573_BIT(7))
		factor = 0x400;
	else if (reg43 & GC573_BIT(6))
		factor = 0x200;
	else if (reg43 & GC573_BIT(5))
		factor = 0x100;
	else
		factor = 0x080;

	return (u32)(((u64)reference_khz * GC573_IT6805_AUDIO_TMDS_SAMPLES *
		      factor) / sample_sum);
}

#endif /* GC573_AUDIO_CLOCK_MATH_H */
