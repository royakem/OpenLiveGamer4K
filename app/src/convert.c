/* SPDX-License-Identifier: GPL-2.0-only */
#include "olg4k/convert.h"
#include <linux/videodev2.h>
#include <string.h>
#include <stdint.h>

static uint8_t clip(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }
static void pixel(int y, int u, int v, bool rec709, bool full, uint8_t *out)
{
    int c = full ? y * 256 : (y - 16) * 298;
    int d = u - 128, e = v - 128;
    int rv = full ? (rec709 ? 403 : 359) : (rec709 ? 459 : 409);
    int gu = full ? (rec709 ? 48 : 88) : (rec709 ? 55 : 100);
    int gv = full ? (rec709 ? 120 : 183) : (rec709 ? 136 : 208);
    int bu = full ? (rec709 ? 475 : 454) : (rec709 ? 541 : 516);
    out[0] = clip((c + rv * e + 128) >> 8);
    out[1] = clip((c - gu * d - gv * e + 128) >> 8);
    out[2] = clip((c + bu * d + 128) >> 8);
}

bool olg4k_frame_to_rgb24_extent(uint32_t fmt, unsigned w, unsigned h,
    unsigned stride, size_t src_len, const uint8_t *src, uint8_t *dst,
    size_t dst_len, uint32_t enc, uint32_t quant)
{
    size_t rowbytes, rows, extent, pixels;
    bool yuv = fmt == V4L2_PIX_FMT_YUYV || fmt == V4L2_PIX_FMT_NV12;
    bool rec709 = enc == V4L2_YCBCR_ENC_709 || enc == V4L2_YCBCR_ENC_DEFAULT;
    bool full = quant == V4L2_QUANTIZATION_FULL_RANGE;
    if (!src || !dst || !w || !h || (size_t)w > SIZE_MAX / h) return false;
    pixels = (size_t)w * h;
    if (pixels > SIZE_MAX / 3 || dst_len < pixels * 3) return false;
    if (fmt != V4L2_PIX_FMT_RGB24 && fmt != V4L2_PIX_FMT_BGR24 && !yuv) return false;
    if (yuv && enc != V4L2_YCBCR_ENC_DEFAULT && enc != V4L2_YCBCR_ENC_709 &&
        enc != V4L2_YCBCR_ENC_601) return false;
    if (yuv && (w & 1)) return false;
    if (fmt == V4L2_PIX_FMT_NV12 && (h & 1)) return false;
    rowbytes = (size_t)w * (fmt == V4L2_PIX_FMT_NV12 ? 1 : yuv ? 2 : 3);
    rows = h;
    if (fmt == V4L2_PIX_FMT_NV12) {
        if (rows > SIZE_MAX - h / 2) return false;
        rows += h / 2;
    }
    if (stride < rowbytes || (size_t)stride > SIZE_MAX / rows) return false;
    extent = (size_t)stride * rows;
    if (src_len < extent) return false;
    for (size_t y = 0; y < h; y++) {
        const uint8_t *row = src + y * stride;
        uint8_t *out = dst + y * w * 3;
        if (fmt == V4L2_PIX_FMT_RGB24) { memcpy(out, row, (size_t)w * 3); continue; }
        for (size_t x = 0; x < w; x++) {
            if (fmt == V4L2_PIX_FMT_BGR24) {
                out[x*3] = row[x*3+2]; out[x*3+1] = row[x*3+1]; out[x*3+2] = row[x*3];
            } else if (fmt == V4L2_PIX_FMT_YUYV) {
                const uint8_t *pair = row + (x / 2) * 4;
                pixel(pair[(x & 1) * 2], pair[1], pair[3], rec709, full, out + x*3);
            } else {
                const uint8_t *uv = src + (size_t)stride * h + (y / 2) * stride + (x & ~(size_t)1);
                pixel(row[x], uv[0], uv[1], rec709, full, out + x*3);
            }
        }
    }
    return true;
}

/* Compatibility API: callers must supply the full stride-based source extent.
 * Real capture uses the explicit extent API above with the mapped length. */
bool olg4k_frame_to_rgb24(uint32_t fmt, unsigned w, unsigned h, unsigned stride,
    const uint8_t *src, uint8_t *dst, size_t dst_len)
{
    size_t rows = h;
    if (fmt == V4L2_PIX_FMT_NV12) {
        if (rows > SIZE_MAX - h / 2) return false;
        rows += h / 2;
    }
    if (!rows || stride > SIZE_MAX / rows) return false;
    return olg4k_frame_to_rgb24_extent(fmt, w, h, stride, (size_t)stride * rows,
        src, dst, dst_len, V4L2_YCBCR_ENC_709, V4L2_QUANTIZATION_LIM_RANGE);
}
bool olg4k_rgb24_swap_red_blue(uint8_t *p, size_t n)
{
    if (!p || n % 3) return false;
    for (size_t i=0; i<n; i+=3) { uint8_t t=p[i]; p[i]=p[i+2]; p[i+2]=t; }
    return true;
}
