# SPDX-License-Identifier: GPL-2.0-only
"""Verify actual shared-reset function stops audio before touching reset MMIO."""
from pathlib import Path
import subprocess,tempfile,unittest
ROOT=Path(__file__).resolve().parents[1]
class ResetTests(unittest.TestCase):
 def test_reset_audio_order(self):
  s=(ROOT/'src/gc573_hw.c').read_text();a=s.index('int gc573_hw_reset_dma(');b=s.index('\nint gc573_hw_init',a)
  pre=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#define BIT(n) (1u<<(n))
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define GC573_REG_DMA_RESET 12
#define msleep(x) ((void)(x))
typedef uint32_t u32;
struct gc573_device { bool audio_running; int mmio; void *audio; };
static int stopped,xruns,writes;
static void gc573_audio_dma_stop(struct gc573_device *d){assert(d->audio_running);stopped++;}
static void gc573_audio_xrun(void *a){(void)a;assert(stopped);xruns++;}
static int gc573_mmio_write32(int *m,u32 r,u32 v){(void)m;(void)r;(void)v;assert(stopped==1 && xruns==1);writes++;return 0;}
static int gc573_mmio_read32(int *m,u32 r,u32 *v){(void)m;(void)r;*v=0;return 0;}
'''
  main=r'''
int main(void){struct gc573_device d={.audio_running=true};
assert(gc573_hw_reset_dma(&d)==0);assert(!d.audio_running && stopped==1 && xruns==1 && writes==3);
assert(gc573_hw_reset_dma(&d)==0);assert(stopped==1 && xruns==1 && writes==6);}
'''
  with tempfile.TemporaryDirectory() as t:
   c=Path(t)/'t.c';exe=Path(t)/'t';c.write_text(pre+s[a:b]+main)
   subprocess.run(['cc','-Wall','-Wextra','-fsanitize=address,undefined',str(c),'-o',str(exe)],check=True)
   subprocess.run([str(exe)],check=True)
if __name__=='__main__':unittest.main()
