/* SPDX-License-Identifier: GPL-2.0-only */
/* Offline unit tests for the V4L2 backend against the mock. */
#include "olg4k/backend.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern const Olg4kBackend olg4k_backend_mock;
extern const Olg4kBackend olg4k_backend_real;

static int failures;

#define CHECK(cond)                                        \
    do {                                                   \
        if (!(cond)) {                                     \
            fprintf(stderr, "FAIL %s:%d: %s\n",           \
                    __FILE__, __LINE__, #cond);            \
            failures++;                                    \
        }                                                  \
    } while (0)

#define CHECK_EQ(a, b) CHECK((a) == (b))

static Olg4kDevice open_mock(void)
{
	Olg4kDevice dev;
	int rc = olg4k_open(&dev, &olg4k_backend_mock, NULL, "/dev/video99",
	                   OLG4K_MODE_STATUS);
	assert(rc == 0);
	return dev;
}

static void test_querycap(void)
{
	Olg4kDevice dev = open_mock();
	struct v4l2_capability cap;
	memset(&cap, 0, sizeof(cap));
	CHECK_EQ(olg4k_querycap(&dev, &cap), 0);
	CHECK_EQ(cap.capabilities & V4L2_CAP_DEVICE_CAPS, V4L2_CAP_DEVICE_CAPS);
	CHECK_EQ(cap.device_caps, V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING);
	CHECK(!strcmp((char *)cap.card, "AVerMedia Live Gamer 4K"));
	CHECK(strstr((char *)cap.driver, "gc573-pure") != NULL);
	olg4k_close(&dev);
}

static void test_enum_fmt(void)
{
	Olg4kDevice dev = open_mock();
	struct v4l2_fmtdesc f;
	unsigned int expected[4] = { V4L2_PIX_FMT_YUYV, V4L2_PIX_FMT_NV12,
	                    V4L2_PIX_FMT_RGB24, V4L2_PIX_FMT_BGR24 };
	unsigned int i;
	for (i = 0; i < 4; i++) {
		CHECK_EQ(olg4k_enum_fmt(&dev, i, &f), 0);
		CHECK_EQ(f.pixelformat, expected[i]);
	}
	CHECK_EQ(olg4k_enum_fmt(&dev, 4, &f), -1);
	/* RGB24 is emulated per the driver contract */
	CHECK_EQ(olg4k_enum_fmt(&dev, 2, &f), 0);
	CHECK_EQ(f.flags & V4L2_FMT_FLAG_EMULATED, V4L2_FMT_FLAG_EMULATED);
	olg4k_close(&dev);
}

static void test_enum_sizes(void)
{
	Olg4kDevice dev = open_mock();
	struct v4l2_frmsizeenum fs;
	unsigned int w[4] = { 3840, 1920, 1280, 1280 };
	unsigned int h[4] = { 2160, 1080, 720, 800 };
	unsigned int i;
	for (i = 0; i < 4; i++) {
		CHECK_EQ(olg4k_enum_framesize(&dev, V4L2_PIX_FMT_YUYV, i, &fs), 0);
		CHECK_EQ(fs.type, V4L2_FRMSIZE_TYPE_DISCRETE);
		CHECK_EQ(fs.discrete.width, w[i]);
		CHECK_EQ(fs.discrete.height, h[i]);
	}
	CHECK_EQ(olg4k_enum_framesize(&dev, V4L2_PIX_FMT_YUYV, 4, &fs), -1);
	/* unsupported fourcc rejected */
	CHECK_EQ(olg4k_enum_framesize(&dev, V4L2_PIX_FMT_MJPEG, 0, &fs), -1);
	olg4k_close(&dev);
}

static void test_enum_intervals(void)
{
	Olg4kDevice dev = open_mock();
	struct v4l2_frmivalenum fi;
	struct v4l2_fract exp[8] = {
		{1,60},{1001,60000},{1,50},{1,30},{1001,30000},{1,25},{1,24},{1001,24000}};
	unsigned int i;
	for (i = 0; i < 8; i++) {
		CHECK_EQ(olg4k_enum_frameinterval(&dev, V4L2_PIX_FMT_NV12, 1920, 1080, i, &fi), 0);
		CHECK_EQ(fi.type, V4L2_FRMIVAL_TYPE_DISCRETE);
		CHECK_EQ(fi.discrete.numerator, exp[i].numerator);
		CHECK_EQ(fi.discrete.denominator, exp[i].denominator);
	}
	CHECK_EQ(olg4k_enum_frameinterval(&dev, V4L2_PIX_FMT_NV12, 1920, 1080, 8, &fi), -1);
	/* size not in the table: rejected */
	CHECK_EQ(olg4k_enum_frameinterval(&dev, V4L2_PIX_FMT_NV12, 640, 480, 0, &fi), -1);
	olg4k_close(&dev);
}

static void test_input(void)
{
	Olg4kDevice dev = open_mock();
	struct v4l2_input in;
	CHECK_EQ(olg4k_enum_input(&dev, 0, &in), 0);
	CHECK_EQ(in.type, V4L2_INPUT_TYPE_CAMERA);
	CHECK(!strncmp((char *)in.name, "HDMI", 5));
	CHECK_EQ(olg4k_enum_input(&dev, 1, &in), -1);
	olg4k_close(&dev);
}

/* --- normalization: exact mirror of gc573_try_format() --- */

static void test_g_fmt_default(void)
{
	Olg4kDevice dev = open_mock();
	struct v4l2_format f;
	memset(&f, 0, sizeof(f));
	f.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	CHECK_EQ(olg4k_g_fmt(&dev, &f), 0);
	CHECK_EQ(f.fmt.pix.width, 1920);
	CHECK_EQ(f.fmt.pix.height, 1080);
	CHECK_EQ(f.fmt.pix.pixelformat, V4L2_PIX_FMT_YUYV);
	CHECK_EQ(f.fmt.pix.bytesperline, 1920 * 2);
	CHECK_EQ(f.fmt.pix.sizeimage, 1920 * 1080 * 2);
	CHECK_EQ(f.fmt.pix.field, V4L2_FIELD_NONE);
	CHECK_EQ(f.fmt.pix.colorspace, V4L2_COLORSPACE_REC709);
	CHECK_EQ(f.fmt.pix.quantization, V4L2_QUANTIZATION_LIM_RANGE);
	olg4k_close(&dev);
}

static void test_try_fmt_nearest(void)
{
	Olg4kDevice dev = open_mock();
	struct v4l2_format f;
	/* nearest size wins: 2000x1100 -> 1920x1080 */
	memset(&f, 0, sizeof(f));
	f.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	f.fmt.pix.width = 2000;
	f.fmt.pix.height = 1100;
	f.fmt.pix.pixelformat = V4L2_PIX_FMT_NV12;
	CHECK_EQ(olg4k_try_fmt(&dev, &f), 0);
	CHECK_EQ(f.fmt.pix.width, 1920);
	CHECK_EQ(f.fmt.pix.height, 1080);
	CHECK_EQ(f.fmt.pix.pixelformat, V4L2_PIX_FMT_NV12);
	CHECK_EQ(f.fmt.pix.bytesperline, 1920);
	CHECK_EQ(f.fmt.pix.sizeimage, 1920 * 1080 * 3 / 2);
	/* unsupported fourcc falls back to YUYV (driver behavior) */
	memset(&f, 0, sizeof(f));
	f.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	f.fmt.pix.width = 3840;
	f.fmt.pix.height = 2160;
	f.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
	CHECK_EQ(olg4k_try_fmt(&dev, &f), 0);
	CHECK_EQ(f.fmt.pix.pixelformat, V4L2_PIX_FMT_YUYV);
	CHECK_EQ(f.fmt.pix.sizeimage, 3840 * 2160 * 2);
	/* hostile dimensions are clamped to 8192 first */
	memset(&f, 0, sizeof(f));
	f.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	f.fmt.pix.width = 65535;
	f.fmt.pix.height = 65535;
	CHECK_EQ(olg4k_try_fmt(&dev, &f), 0);
	CHECK_EQ(f.fmt.pix.width, 3840);
	CHECK_EQ(f.fmt.pix.height, 2160);
	/* note: the core API forces V4L2_BUF_TYPE_VIDEO_CAPTURE, so wrong-type
	 * rejection is an internal driver detail not reachable here. */
	olg4k_close(&dev);
}

static void test_s_fmt_preview_only(void)
{
	Olg4kDevice dev = open_mock();
	struct v4l2_format f;
	memset(&f, 0, sizeof(f));
	f.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	f.fmt.pix.width = 1280;
	f.fmt.pix.height = 720;
	f.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
	/* read-only mode: S_FMT must be muted */
	CHECK_EQ(olg4k_s_fmt(&dev, &f), -1);
	CHECK_EQ(dev.muting_ioctls, 1);
	CHECK_EQ(olg4k_streamon(&dev, V4L2_BUF_TYPE_VIDEO_CAPTURE), -1);
	CHECK_EQ(dev.muting_ioctls, 2);
	/* switch to preview mode: same calls succeed */
	olg4k_set_mode(&dev, OLG4K_MODE_PREVIEW);
	CHECK_EQ(olg4k_s_fmt(&dev, &f), 0);
	CHECK_EQ(f.fmt.pix.width, 1280);
	CHECK_EQ(f.fmt.pix.height, 720);
	CHECK_EQ(dev.muting_ioctls, 2);
	/* g_fmt reflects the new format */
	memset(&f, 0, sizeof(f));
	f.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	CHECK_EQ(olg4k_g_fmt(&dev, &f), 0);
	CHECK_EQ(f.fmt.pix.width, 1280);
	CHECK_EQ(f.fmt.pix.sizeimage, 1280 * 720 * 2);
	olg4k_close(&dev);
}

static void test_s_parm_nearest(void)
{
	Olg4kDevice dev;
	assert(olg4k_open(&dev, &olg4k_backend_mock, NULL, "/dev/video99",
	                OLG4K_MODE_PREVIEW) == 0);
	struct v4l2_streamparm p;
	memset(&p, 0, sizeof(p));
	p.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	/* request 30.03fps (1001/33000): nearest is 1/30 (30fps), closer than
	 * 1001/30000 (29.97fps) — same cross-multiply math as the driver. */
	p.parm.capture.timeperframe.numerator = 1001;
	p.parm.capture.timeperframe.denominator = 33000;
	CHECK_EQ(olg4k_s_parm(&dev, &p), 0);
	CHECK_EQ(p.parm.capture.timeperframe.numerator, 1);
	CHECK_EQ(p.parm.capture.timeperframe.denominator, 30);
	/* g_parm reports it */
	memset(&p, 0, sizeof(p));
	p.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	CHECK_EQ(olg4k_g_parm(&dev, &p), 0);
	CHECK_EQ(p.parm.capture.capability, V4L2_CAP_TIMEPERFRAME);
	CHECK_EQ(p.parm.capture.timeperframe.numerator, 1);
	CHECK_EQ(p.parm.capture.timeperframe.denominator, 30);
	/* zero timeperframe -> index 0 (60 fps) */
	memset(&p, 0, sizeof(p));
	p.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	CHECK_EQ(olg4k_s_parm(&dev, &p), 0);
	CHECK_EQ(p.parm.capture.timeperframe.numerator, 1);
	CHECK_EQ(p.parm.capture.timeperframe.denominator, 60);
	olg4k_close(&dev);
}

/* --- streaming lifecycle against the mock --- */

static void test_streaming_lifecycle(void)
{
	Olg4kDevice dev;
	int rc = olg4k_open(&dev, &olg4k_backend_mock, NULL, "/dev/video99",
	                   OLG4K_MODE_PREVIEW);
	assert(rc == 0);
	struct v4l2_format f;
	struct v4l2_requestbuffers rb;
	struct v4l2_buffer b;
	unsigned int i;
	time_t seen;
	int got;

	/* 1280x720 NV12 @ 30fps */
	memset(&f, 0, sizeof(f));
	f.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	f.fmt.pix.width = 1280;
	f.fmt.pix.height = 720;
	f.fmt.pix.pixelformat = V4L2_PIX_FMT_NV12;
	CHECK_EQ(olg4k_s_fmt(&dev, &f), 0);
	memset(&f, 0, sizeof(f));
	f.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	CHECK_EQ(olg4k_g_fmt(&dev, &f), 0);
	CHECK_EQ(f.fmt.pix.sizeimage, 1280 * 720 * 3 / 2);
	{
		struct v4l2_streamparm p;
		memset(&p, 0, sizeof(p));
		p.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		p.parm.capture.timeperframe.numerator = 1;
		p.parm.capture.timeperframe.denominator = 30;
		CHECK_EQ(olg4k_s_parm(&dev, &p), 0);
	}

	/* buffers */
	memset(&rb, 0, sizeof(rb));
	rb.count = 4;
	CHECK_EQ(olg4k_reqbufs(&dev, &rb), 0);
	CHECK_EQ(rb.count, 4);
	CHECK_EQ(rb.memory, V4L2_MEMORY_MMAP);

	/* map the pool once (mock arena), queue all buffers */
	char *map = NULL;
	struct v4l2_buffer qb;
	memset(&b, 0, sizeof(b));
	for (i = 0; i < 4; i++) {
		memset(&qb, 0, sizeof(qb)); qb.index = i;
		CHECK_EQ(olg4k_querybuf(&dev, &qb), 0);
		CHECK_EQ(olg4k_mmap(&dev, (void **)&map, qb.length, 3, qb.m.offset), 0);
		CHECK(map != NULL);
		CHECK_EQ(olg4k_munmap(&dev, map, qb.length), 0);
	}
	for (i = 0; i < 4; i++) {
		memset(&b, 0, sizeof(b));
		b.index = i;
		CHECK_EQ(olg4k_qbuf(&dev, &b), 0);
	}

	/* start */
	CHECK_EQ(olg4k_streamon(&dev, V4L2_BUF_TYPE_VIDEO_CAPTURE), 0);
	CHECK(dev.streaming);

	/* dequeue 12 frames: every index is valid, timestamps non-decreasing */
	got = 0;
	seen = 0;
	for (i = 0; i < 12; i++) {
		memset(&b, 0, sizeof(b));
		int r = olg4k_dqbuf(&dev, &b);
		CHECK_EQ(r, 0);
		CHECK(b.index < 4);
		CHECK_EQ(b.bytesused, 1280 * 720 * 3 / 2);
		CHECK_EQ(b.memory, V4L2_MEMORY_MMAP);
					if (i > 0)
						CHECK(b.timestamp.tv_sec > seen ||
						      (b.timestamp.tv_sec == seen && b.timestamp.tv_usec >= 0));
		seen = b.timestamp.tv_sec;
		got++;
		/* requeue for continuous streaming */
		memset(&b, 0, sizeof(b));
		b.index = i % 4;
		CHECK_EQ(olg4k_qbuf(&dev, &b), 0);
	}
	CHECK_EQ(got, 12);

	/* stop: STREAMOFF must succeed and clear state */
	CHECK_EQ(olg4k_streamoff(&dev, V4L2_BUF_TYPE_VIDEO_CAPTURE), 0);
	CHECK(!dev.streaming);

	olg4k_close(&dev);
}

static void test_streamon_requires_buffers(void)
{
	Olg4kDevice dev;
	int rc = olg4k_open(&dev, &olg4k_backend_mock, NULL, "/dev/video99",
	                   OLG4K_MODE_PREVIEW);
	assert(rc == 0);
	/* no buffers requested: STREAMON must fail */
	CHECK_EQ(olg4k_streamon(&dev, V4L2_BUF_TYPE_VIDEO_CAPTURE), -1);
	CHECK(!dev.streaming);
	olg4k_close(&dev);
}

static void test_close_idempotent(void)
{
	Olg4kDevice dev;
	int rc = olg4k_open(&dev, &olg4k_backend_mock, NULL, "/dev/video99",
	                   OLG4K_MODE_STATUS);
	assert(rc == 0);
	olg4k_close(&dev);
	CHECK_EQ(dev.fd, -1);
}

static void test_real_backend_symbols(void)
{
	/* real backend must be present and self-describe */
	CHECK(olg4k_backend_real.open != NULL);
	CHECK(olg4k_backend_real.ioctl != NULL);
	CHECK(olg4k_backend_real.mmap != NULL);
	CHECK(!strcmp(olg4k_backend_real.name, "direct-v4l2"));
	CHECK(!strcmp(olg4k_backend_mock.name, "mock-gc573"));
}

static void test_err_strings(void)
{
	CHECK(!strcmp(olg4k_err_string(OLG4K_ERR_OK), "ok"));
	CHECK(!strcmp(olg4k_err_string(OLG4K_ERR_BUSY), "device busy"));
	CHECK(olg4k_err_string((Olg4kErr)99) != NULL);
}

int main(void)
{
	test_querycap();
	test_enum_fmt();
	test_enum_sizes();
	test_enum_intervals();
	test_input();
	test_g_fmt_default();
	test_try_fmt_nearest();
	test_s_fmt_preview_only();
	test_s_parm_nearest();
	test_streaming_lifecycle();
	test_streamon_requires_buffers();
	test_close_idempotent();
	test_real_backend_symbols();
	test_err_strings();

	if (failures) {
		fprintf(stderr, "%d CHECK(s) FAILED\n", failures);
		return 1;
	}
	printf("all backend contract tests passed\n");
	return 0;
}
