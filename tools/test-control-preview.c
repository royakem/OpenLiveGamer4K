/* SPDX-License-Identifier: GPL-2.0-only */
/* Explicit hardware test; never run by the offline test suite. */
#include "olg4k/backend.h"
#include "olg4k/worker.h"
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
extern const Olg4kBackend olg4k_backend_real;
static atomic_uint frames;
static atomic_uint varied;
static void frame(struct Olg4kWorker *w, const uint8_t *p,
                  unsigned width, unsigned height, void *data)
{
    (void)w; (void)data;
    size_t n = (size_t)width * height * 3;
    for (size_t i = 1; i < n; i++)
        if (p[i] != p[0]) { atomic_store(&varied, 1); break; }
    atomic_fetch_add(&frames, 1);
}
int main(int argc, char **argv)
{
    if (argc != 2) { fprintf(stderr, "Usage: %s /dev/videoN\n", argv[0]); return 2; }
    const uint32_t formats[] = {V4L2_PIX_FMT_YUYV, V4L2_PIX_FMT_NV12,
        V4L2_PIX_FMT_RGB24, V4L2_PIX_FMT_BGR24};
    for (unsigned k = 0; k < 4; k++) {
        Olg4kDevice dev;
        struct Olg4kWorker *worker = NULL;
        struct v4l2_capability cap = {0};
        struct v4l2_format fmt = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE};
        struct v4l2_streamparm rate = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE};
        if (olg4k_open(&dev, &olg4k_backend_real, NULL, argv[1], OLG4K_MODE_PREVIEW)) return 1;
        if (olg4k_querycap(&dev, &cap) || strcmp((char*)cap.driver, "gc573-pure")) {
            fprintf(stderr, "Refusing a device that is not gc573_pure\n"); olg4k_close(&dev); return 1;
        }
        fmt.fmt.pix.width=1280; fmt.fmt.pix.height=720; fmt.fmt.pix.pixelformat=formats[k];
        rate.parm.capture.timeperframe.numerator=1; rate.parm.capture.timeperframe.denominator=60;
        if (olg4k_s_fmt(&dev,&fmt) || olg4k_s_parm(&dev,&rate)) { olg4k_close(&dev); return 1; }
        atomic_store(&frames, 0); atomic_store(&varied, 0);
        if (!olg4k_worker_prepare(&worker,&dev,4,frame,NULL) || !olg4k_worker_start(worker)) {
            fprintf(stderr,"prepare/start failed\n"); if(worker) olg4k_worker_free(worker); olg4k_close(&dev); return 1;
        }
        for (unsigned t=0;t<100 && atomic_load(&frames)<120;t++) {
            if (olg4k_worker_state(worker)==OLG4K_WORKER_ERROR) break;
            usleep(100000);
        }
        olg4k_worker_stop(worker);
        unsigned count=atomic_load(&frames), different=atomic_load(&varied);
        printf("format=%c%c%c%c frames=%u varied=%u state=%d\n",formats[k]&255,(formats[k]>>8)&255,(formats[k]>>16)&255,(formats[k]>>24)&255,count,different,olg4k_worker_state(worker));
        olg4k_worker_free(worker); olg4k_close(&dev);
        if(count<120 || !different) return 1;
    }
    return 0;
}
