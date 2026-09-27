/* SPDX-License-Identifier: GPL-2.0-only */
/* Offline unit tests for the frame-conversion helpers. */
#include "olg4k/convert.h"

#include <stdio.h>
#include <linux/videodev2.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(cond)                                                  \
    do {                                                             \
        if (!(cond)) {                                               \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                              \
        }                                                            \
    } while (0)

static void test_rgb24_passthrough(void)
{
	const uint32_t RGB24 = V4L2_PIX_FMT_RGB24;
	unsigned int w = 4, h = 2;
	uint8_t src[4 * 2 * 3];
	uint8_t dst[4 * 2 * 3];
	size_t i;
	for (i = 0; i < sizeof(src); i++)
		src[i] = (uint8_t)(i * 7);
	CHECK(olg4k_frame_to_rgb24(RGB24, w, h, 4 * 3, src, dst, sizeof(dst)));
	CHECK(!memcmp(src, dst, sizeof(src)));
	/* stride wider than the frame: bytesperline padding must be skipped */
	uint8_t padded[2 * (4 * 3 + 16)];
	memset(padded, 0xEE, sizeof(padded));
	memcpy(padded + 0, src, 4 * 3);
	memcpy(padded + (4 * 3 + 16), src + 4 * 3, 4 * 3);
	CHECK(olg4k_frame_to_rgb24(RGB24, w, h, 4 * 3 + 16, padded, dst, sizeof(dst)));
	CHECK(!memcmp(src, dst, sizeof(src)));
}

static void test_swap_red_blue(void)
{
	uint8_t px[3] = { 10, 200, 300 & 0xFF };
	/* R=10, G=200, B=44 */
	px[2] = 44;
	CHECK(olg4k_rgb24_swap_red_blue(px, 3));
	CHECK(px[0] == 44 && px[1] == 200 && px[2] == 10);
	/* idempotent-ish: swap again restores */
	CHECK(olg4k_rgb24_swap_red_blue(px, 3));
	CHECK(px[0] == 10 && px[2] == 44);
	/* invalid length rejected */
	CHECK(!olg4k_rgb24_swap_red_blue(px, 4));
	CHECK(!olg4k_rgb24_swap_red_blue(NULL, 3));
}

static void test_yuyv_known_pixels(void)
{
	/* 4x1 YUYV row: gray pair (Y=128 U=128 V=128) then blue pair
	 * (Y=16 U=240 V=16). */
	const uint32_t YUYV = 0x56595559;
	uint8_t src[8] = { 128, 128, 128, 128, 16, 240, 16, 16 };
	uint8_t dst[4 * 3];
	memset(dst, 0xAA, sizeof(dst));
	CHECK(olg4k_frame_to_rgb24(YUYV, 4, 1, 8, src, dst, sizeof(dst)));
	/* mid gray: all channels ~128 */
	CHECK(dst[0] >= 120 && dst[0] <= 136);
	CHECK(dst[1] >= 120 && dst[1] <= 136);
	CHECK(dst[2] >= 120 && dst[2] <= 136);
	/* saturated blue: B high, R low */
	CHECK(dst[6 + 2] >= 200);
	CHECK(dst[6 + 0] <= 60);
	CHECK(dst[6 + 1] <= 60);
	/* second pixel of the pair shares U/V */
	CHECK(dst[9 + 2] >= 200);
}

static void test_nv12_known_pixels(void)
{
	/* 2x2 NV12 frame: all Y=16 (black), U=V=128 -> black RGB */
	const uint32_t NV12 = 0x3231564E;
	uint8_t yplane[2 * 2];
	uint8_t uv[2 * 1 * 2];
	uint8_t src[2 * 2 + 4];
	memset(yplane, 16, sizeof(yplane));
	memset(uv, 128, sizeof(uv));
	memcpy(src, yplane, 4);
	memcpy(src + 4, uv, 4);
	uint8_t dst[2 * 2 * 3];
	memset(dst, 0xFF, sizeof(dst));
	CHECK(olg4k_frame_to_rgb24(NV12, 2, 2, 2, src, dst, sizeof(dst)));
	for (size_t i = 0; i < sizeof(dst); i += 3) {
		CHECK(dst[i] <= 8);
		CHECK(dst[i + 1] <= 8);
		CHECK(dst[i + 2] <= 8);
	}
	/* odd height rejected */
	CHECK(!olg4k_frame_to_rgb24(NV12, 2, 3, 2, src, dst, sizeof(dst)));
}

static void test_rejects(void)
{
	uint8_t buf[64] = { 0 };
	CHECK(!olg4k_frame_to_rgb24(0x42, 4, 2, 8, buf, buf, sizeof(buf))); /* unknown fourcc */
	CHECK(!olg4k_frame_to_rgb24(V4L2_PIX_FMT_RGB24, 4, 2, 4, buf, buf, sizeof(buf))); /* short stride */
	CHECK(!olg4k_frame_to_rgb24(V4L2_PIX_FMT_RGB24, 0, 2, 0, buf, buf, sizeof(buf))); /* zero width */
	CHECK(!olg4k_frame_to_rgb24(V4L2_PIX_FMT_RGB24, 4, 2, 12, buf, NULL, 0)); /* no dst */
	CHECK(!olg4k_frame_to_rgb24(V4L2_PIX_FMT_RGB24, 4, 2, 12, NULL, buf, sizeof(buf))); /* no src */
}

static void test_extents_and_colors(void)
{
    uint8_t *nv = malloc(12), out[24], padded[24], expect[24];
    memset(nv, 16, 8); memset(nv+8, 128, 4);
    CHECK(olg4k_frame_to_rgb24_extent(V4L2_PIX_FMT_NV12,4,2,4,12,nv,out,24,V4L2_YCBCR_ENC_709,V4L2_QUANTIZATION_LIM_RANGE));
    for (unsigned i=0;i<24;i++) CHECK(out[i]==0);
    CHECK(!olg4k_frame_to_rgb24_extent(V4L2_PIX_FMT_NV12,4,2,4,11,nv,out,24,0,0));
    CHECK(!olg4k_frame_to_rgb24_extent(V4L2_PIX_FMT_NV12,4,2,4,12,nv,out,23,0,0));
    memset(padded,0xee,24); memcpy(padded,nv,4); memcpy(padded+8,nv+4,4); memcpy(padded+16,nv+8,4);
    CHECK(olg4k_frame_to_rgb24_extent(V4L2_PIX_FMT_NV12,4,2,8,24,padded,expect,24,0,0));
    CHECK(!memcmp(out,expect,24));
    free(nv);
    const uint8_t red709[4]={63,102,63,240};
    CHECK(olg4k_frame_to_rgb24_extent(V4L2_PIX_FMT_YUYV,2,1,4,4,red709,out,24,V4L2_YCBCR_ENC_709,V4L2_QUANTIZATION_LIM_RANGE));
    CHECK(out[0]>=250 && out[1]<=3 && out[2]<=3);
    const uint8_t fullred[4]={54,99,54,255};
    CHECK(olg4k_frame_to_rgb24_extent(V4L2_PIX_FMT_YUYV,2,1,4,4,fullred,out,24,V4L2_YCBCR_ENC_709,V4L2_QUANTIZATION_FULL_RANGE));
    CHECK(out[0]>=250 && out[1]<=3 && out[2]<=3);
    CHECK(!olg4k_frame_to_rgb24_extent(V4L2_PIX_FMT_YUYV,1,1,4,4,red709,out,24,0,0));
    const uint8_t bgr[6]={255,0,0,0,0,255};
    CHECK(olg4k_frame_to_rgb24_extent(V4L2_PIX_FMT_BGR24,2,1,6,6,bgr,out,24,0,0));
    CHECK(out[0]==0 && out[2]==255 && out[3]==255 && out[5]==0);
}

int main(void)
{
	test_extents_and_colors();
	test_rgb24_passthrough();
	test_swap_red_blue();
	test_yuyv_known_pixels();
	test_nv12_known_pixels();
	test_rejects();
	if (failures) {
		fprintf(stderr, "%d CHECK(s) FAILED\n", failures);
		return 1;
	}
	printf("all conversion tests passed\n");
	return 0;
}
