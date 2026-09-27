#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Offline checks for the shared GC573 pixel format layout helper."""
import pathlib
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]

HARNESS = r'''
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include "gc573_formats.h"
'''

def main():
    with tempfile.TemporaryDirectory() as td:
        d = pathlib.Path(td)
        (d / "linux").mkdir()
        (d / "linux/types.h").write_text("#include <stdint.h>\n#include <stddef.h>\ntypedef uint32_t u32;\ntypedef uint8_t u8;\n#define U32_MAX UINT32_MAX\n")
        (d / "linux/overflow.h").write_text("#define check_mul_overflow(a,b,p) __builtin_mul_overflow((a),(b),(p))\n")
        (d / "linux/videodev2.h").write_text("#define FMT(a,b,c,d) ((uint32_t)(a)|((uint32_t)(b)<<8)|((uint32_t)(c)<<16)|((uint32_t)(d)<<24))\n#define V4L2_PIX_FMT_YUYV FMT('Y','U','Y','V')\n#define V4L2_PIX_FMT_NV12 FMT('N','V','1','2')\n#define V4L2_PIX_FMT_RGB24 FMT('R','G','B','3')\n#define V4L2_PIX_FMT_BGR24 FMT('B','G','R','3')\n")
        c = d / "check.c"
        c.write_text(HARNESS + r'''
int main(void) {
 const uint32_t fs[] = {V4L2_PIX_FMT_YUYV,V4L2_PIX_FMT_NV12,V4L2_PIX_FMT_RGB24,V4L2_PIX_FMT_BGR24};
 const uint32_t wh[][2] = {{3840,2160},{1920,1080},{1280,720},{1280,800}};
 const uint32_t strides[][4] = {{7680,3840,11520,11520},{3840,1920,5760,5760},{2560,1280,3840,3840},{2560,1280,3840,3840}};
 const uint32_t sizes[][4] = {{16588800,12441600,24883200,24883200},{4147200,3110400,6220800,6220800},{1843200,1382400,2764800,2764800},{2048000,1536000,3072000,3072000}};
 unsigned i,j; uint32_t s,z;
 for(i=0;i<4;i++) for(j=0;j<4;j++) {
  if(gc573_format_layout(fs[i],wh[j][0],wh[j][1],&s,&z) || s!=strides[j][i] || z!=sizes[j][i]) return 1;
 }
 if(gc573_format_layout(fs[0],0,2,&s,&z)!=-EINVAL) return 2;
 if(gc573_format_layout(UINT32_C(0x30313050),2,2,&s,&z)!=-EINVAL) return 3;
 if(gc573_format_layout(fs[0],UINT32_MAX-1,2,&s,&z)!=-EOVERFLOW) return 4;
 if(gc573_format_layout(fs[0],2,2,0,&z)!=-EINVAL) return 5;
 { uint8_t px[]={0x11,0x22,0x33,0xaa,0xbb,0xcc}, original[]={0x11,0x22,0x33,0xaa,0xbb,0xcc};
   if(gc573_rgb24_swap_red_blue(px,sizeof(px)) || px[0]!=0x33 || px[1]!=0x22 || px[2]!=0x11 || px[3]!=0xcc || px[4]!=0xbb || px[5]!=0xaa) return 6;
   if(gc573_rgb24_swap_red_blue(px,sizeof(px)) || __builtin_memcmp(px,original,sizeof(px))) return 7;
   if(gc573_rgb24_swap_red_blue(px,5)!=-EINVAL) return 8;
   /* Only DONE enters the conversion path in buf_finish; errors preserve bytes. */
   if(__builtin_memcmp(px,original,sizeof(px))) return 9;
 }
 return 0;
}
''')
        exe = d / "check"
        subprocess.run(["cc", "-std=c11", "-Wall", "-Werror", "-I", str(d), "-I", str(ROOT / "src"), str(c), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
    print("format layout checks passed")

if __name__ == "__main__":
    main()
