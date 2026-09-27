/* SPDX-License-Identifier: GPL-2.0-only */
/* Offline unit test for the preview worker against the deterministic mock.
 * Proves the DQBUF thread, frame conversion, and clean stop/release path
 * without a display or a real device. */
#include "olg4k/backend.h"
#include "olg4k/worker.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

extern const Olg4kBackend olg4k_backend_mock;

static int failures;
#define CHECK(cond)                                              \
    do {                                                         \
        if (!(cond)) {                                           \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                          \
        }                                                        \
    } while (0)

static int frames_seen;
static int first_frame_ok;
static int last_rgb_size;

static void on_frame(struct Olg4kWorker *w, const uint8_t *rgb,
                     unsigned int width, unsigned int height, void *user)
{
    (void)w;
    (void)user;
    frames_seen++;
    if (frames_seen == 1) {
        /* first frame: pattern is non-constant and the buffer is full */
        unsigned long long sum = 0;
        size_t len = (size_t)width * height * 3;
        int all_same = 1;
        uint8_t ref = rgb[0];
        for (size_t i = 0; i < len; i++) {
            sum += rgb[i];
            if (rgb[i] != ref)
                all_same = 0;
        }
        first_frame_ok = (!all_same) && (len > 0);
        last_rgb_size = (int)len;
    }
}

int main(void)
{
    Olg4kDevice dev;
    struct Olg4kWorker *worker = NULL;

    assert(olg4k_open(&dev, &olg4k_backend_mock, NULL, "/dev/mock0",
                      OLG4K_MODE_PREVIEW) == 0);

    /* 1280x720 NV12 @ 30fps */
    {
        struct v4l2_format f;
        struct v4l2_streamparm p;
        memset(&f, 0, sizeof(f));
        f.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        f.fmt.pix.width = 1280;
        f.fmt.pix.height = 720;
        f.fmt.pix.pixelformat = V4L2_PIX_FMT_NV12;
        assert(olg4k_s_fmt(&dev, &f) == 0);
        memset(&p, 0, sizeof(p));
        p.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        p.parm.capture.timeperframe.numerator = 1;
        p.parm.capture.timeperframe.denominator = 30;
        assert(olg4k_s_parm(&dev, &p) == 0);
    }

    CHECK(olg4k_worker_prepare(&worker, &dev, 4, on_frame, NULL));
    CHECK(worker != NULL);
    CHECK(olg4k_worker_state(worker) == OLG4K_WORKER_IDLE);
    CHECK(olg4k_worker_start(worker));
    CHECK(olg4k_worker_state(worker) == OLG4K_WORKER_RUNNING);

    /* let it run; the mock retires buffers immediately so frames flow fast */
    for (int i = 0; i < 200 && frames_seen < 20; i++)
        usleep(1000);

    CHECK(frames_seen >= 1);
    CHECK(first_frame_ok);
    CHECK(last_rgb_size == 1280 * 720 * 3);
    CHECK(olg4k_worker_frames(worker) >= 1);

    olg4k_worker_stop(worker);
    CHECK(olg4k_worker_state(worker) == OLG4K_WORKER_IDLE);
    CHECK(olg4k_worker_error(worker)[0] == '\0');

    /* the device must be reusable after stop (mode still preview) */
    {
        struct v4l2_format f;
        memset(&f, 0, sizeof(f));
        f.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        CHECK(olg4k_g_fmt(&dev, &f) == 0);
        CHECK(f.fmt.pix.width == 1280);
    }

    olg4k_worker_free(worker);
    worker = NULL;
    olg4k_close(&dev);

    if (failures) {
        fprintf(stderr, "%d CHECK(s) FAILED\n", failures);
        return 1;
    }
    printf("all worker tests passed (frames_seen=%d)\n", frames_seen);
    return 0;
}
