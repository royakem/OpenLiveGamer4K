/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Offline unit test for the shared IT6805 audio clock math
 * (src/gc573_audio_clock_math.h). It compiles the same header the kernel
 * module uses, so the assertions below exercise the exact code the driver
 * runs — not a copy. Run via tests/test_audio_clock_math.py (offline) or
 * directly: gcc -std=c99 -Wall -Wextra test_audio_clock_math.c -o t && ./t
 *
 * Expected values are the reference it6805_audio_rate_code bin table and the
 * vendor iTE6805_Enable_Audio_Output decode, cross-checked in Python.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
#define BIT(n) (1u << (n))

#include "gc573_audio_clock_math.h"

static int failures;
#define CHECK(cond, msg) do { \
	if (!(cond)) { \
		fprintf(stderr, "FAIL: %s (%s)\n", (msg), #cond); \
		failures++; \
	} else { \
		fprintf(stderr, "ok:   %s\n", (msg)); \
	} \
} while (0)

static void test_rate_code_table(void)
{
	/* Each nominal rate must map to its IT6805 code (kHz units). */
	CHECK(gc573_it6805_audio_rate_code(48) == 0x02, "48 kHz -> 0x02");
	CHECK(gc573_it6805_audio_rate_code(44) == 0x00, "44.1 kHz -> 0x00");
	CHECK(gc573_it6805_audio_rate_code(32) == 0x03, "32 kHz -> 0x03");
	CHECK(gc573_it6805_audio_rate_code(96) == 0x0a, "96 kHz -> 0x0a");
	CHECK(gc573_it6805_audio_rate_code(192) == 0x0e, "192 kHz -> 0x0e");
	CHECK(gc573_it6805_audio_rate_code(384) == 0x05, "384 kHz -> 0x05");
	/* Bin boundaries are inclusive. */
	CHECK(gc573_it6805_audio_rate_code(46) == 0x02, "46 kHz -> 48k bin");
	CHECK(gc573_it6805_audio_rate_code(39) == 0x00, "39 kHz -> 44.1k bin");
	CHECK(gc573_it6805_audio_rate_code(26) == 0x03, "26 kHz -> 32k bin");
	CHECK(gc573_it6805_audio_rate_code(58) == 0x02, "58 kHz -> 48k bin");
	/* Out-of-range maps to the 1024 kHz code (reference behavior). */
	CHECK(gc573_it6805_audio_rate_code(25) == 0x35, "25 kHz -> 1024k code");
	CHECK(gc573_it6805_audio_rate_code(900) == 0x35, "900 kHz -> 1024k code");
}

static void test_receiver_decode(void)
{
	/* Vendor functional decode: (B6>>2 & 0x30) | (B5 & 0x0f).
	 * Observed on the test card (audio_status snapshots):
	 *   locked 4K60 48k source: b5=02 b6=0b -> (0x0b>>2)&0x30|0x02 = 0x02
	 *   (the 48 kHz code — the match case the start op expects)
	 *   previously observed locked:                 b5=02 b6=db -> 0x32 (no defined rate code;
	 *   0x32 is NOT the 32 kHz code — that is 0x03). Decoding an previously observed locked
	 *   receiver to an undefined code is expected and must not be read as
	 *   "32k internal". */
	CHECK(gc573_it6805_decode_receiver_rate(0x02, 0x0b) == 0x02,
	      "observed locked b5=02 b6=0b -> 0x02 (48k internal, match case)");
	CHECK(gc573_it6805_decode_receiver_rate(0x02, 0xdb) == 0x32,
	      "observed previously observed locked b5=02 b6=db -> 0x32 (undefined code, not 32k)");
	CHECK(gc573_it6805_decode_receiver_rate(0x02, 0x02) == 0x02,
	      "b5=02 b6=02 -> 0x02 (48k internal)");
	CHECK(gc573_it6805_decode_receiver_rate(0x00, 0x00) == 0x00,
	      "b5=00 b6=00 -> 0x00 (44.1k internal)");
	/* B6 upper nibble only contributes via bits [5:2]. */
	CHECK(gc573_it6805_decode_receiver_rate(0x0f, 0x07) == 0x0f,
	      "b5 low nibble preserved, b6=07 -> 0x0f");
	/* The 32 kHz code is 0x03, per the rate table below. */
	CHECK(gc573_it6805_audio_rate_code(32) == 0x03,
	      "32 kHz code is 0x03, never 0x32");
}

static void test_compute_rate(void)
{
	/* rate_khz = tmds_khz * N / (CTS << 7); 48 kHz/192fs gives 48 kHz. */
	CHECK(gc573_it6805_compute_rate_khz(27000, 128000, 562500) == 48,
	      "tmds=27MHz N=128000 CTS=562500 -> 48 kHz");
	CHECK(gc573_it6805_compute_rate_khz(1000, 100, 0) == 0,
	      "CTS=0 -> 0 (no divide)");
}

static void test_tmds_clock(void)
{
	/* clock = reference * 10 * factor / sample_sum. */
	CHECK(gc573_it6805_tmds_clock_khz(19000, 0x00, 1600) == 15200,
	      "ref=19MHz factor=0x080 sum=1600 -> 15200 kHz");
	CHECK(gc573_it6805_tmds_clock_khz(19000, 0x80, 3200) == 60800,
	      "ref=19MHz factor=0x400 sum=3200 -> 60800 kHz");
	CHECK(gc573_it6805_tmds_clock_khz(19000, 0x40, 3200) == 30400,
	      "ref=19MHz factor=0x200 sum=3200 -> 30400 kHz");
	CHECK(gc573_it6805_tmds_clock_khz(19000, 0x20, 3200) == 15200,
	      "ref=19MHz factor=0x100 sum=3200 -> 15200 kHz");
	CHECK(gc573_it6805_tmds_clock_khz(0, 0x40, 100) == 0,
	      "ref=0 -> 0");
	CHECK(gc573_it6805_tmds_clock_khz(19000, 0x40, 0) == 0,
	      "sum=0 -> 0 (no divide)");
}

int main(void)
{
	test_rate_code_table();
	test_receiver_decode();
	test_compute_rate();
	test_tmds_clock();

	if (failures) {
		fprintf(stderr, "%d FAILURE(S)\n", failures);
		return 1;
	}
	fprintf(stderr, "ALL PASS\n");
	return 0;
}
