# SPDX-License-Identifier: GPL-2.0-only
"""Compile actual receiver control functions against deterministic I2C faults."""
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCE = (ROOT / 'src/gc573_receiver.c').read_text()

def function(name):
    start = SOURCE.index(('int ' if name == 'gc573_receiver_prepare_audio' else 'static int ') + name + '(')
    body = SOURCE.index('{', start)
    depth = 1
    end = body + 1
    while depth:
        depth += (SOURCE[end] == '{') - (SOURCE[end] == '}')
        end += 1
    return SOURCE[start:end]

PRELUDE = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <errno.h>
#include <stdio.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
#define BIT(n) (1u << (n))
#define dev_info(...) ((void)0)
#define dev_warn(...) ((void)0)
#define dev_err(...) ((void)0)
#define GC573_IT6805_BANK_0 0
#define GC573_IT6805_BANK_2 2
struct gc573_device { int unused; };
#include "gc573_audio_clock_math.h"
static int bank, reads, writes, bank_fail, fail_read, update_fail, overrides;
static unsigned updates, polls;
static u8 regs[256];
static int gc573_it6805_set_bank(struct gc573_device *d, u8 b) {
 (void)d; if (bank_fail) return -EIO; bank=b; return 0;
}
static int gc573_it6805_read(struct gc573_device *d, u8 r,u8 *v) {
 (void)d; reads++; if(fail_read && reads==fail_read)return -EIO; *v=regs[r]; return 0;
}
static int gc573_it6805_update_bits(struct gc573_device *d,u8 r,u8 m,u8 v) {
 (void)d; updates++; if(update_fail && updates==(unsigned)update_fail)return -EIO;
 assert(bank==0); writes++; if(r==0x81 && (v&64))overrides++;
 regs[r]=(regs[r]&~m)|(v&m); return 0;
}
static void reset(void) {
 bank=reads=writes=bank_fail=fail_read=update_fail=overrides=0;
 updates=polls=0;
 for(int i=0;i<256;i++)regs[i]=0;
 regs[0x19]=0x80; regs[0xb5]=2;
}
'''

class SequenceTests(unittest.TestCase):
    def compile_run(self, source):
        with tempfile.TemporaryDirectory() as tmp:
            c = pathlib.Path(tmp)/'test.c'
            binary=pathlib.Path(tmp)/'test'
            c.write_text(source)
            subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Wno-unused-function',
                            '-fsanitize=address,undefined','-I'+str(ROOT/'src'),str(c),'-o',str(binary)],check=True)
            subprocess.run([str(binary)],check=True)

    def test_clock_control(self):
        self.compile_run(PRELUDE + r'''
static u32 test_n=6144,test_cts=148500,test_tmds=148500;
static int reset_error;
static int gc573_it6805_read_ncts(struct gc573_device *d,u32 *n,u32 *cts) {
 (void)d; polls++; *n=test_n; *cts=test_cts;return 0;
}
static int gc573_it6805_measure_audio_tmds(struct gc573_device *d,u32 *v) {
 (void)d; *v=test_tmds;return 0;
}
static int gc573_it6805_reset_audio_logic(struct gc573_device *d){(void)d;return reset_error;}
''' + function('gc573_receiver_update_audio_clock') + r'''
int main(void){struct gc573_device d={0};
 reset(); assert(gc573_receiver_update_audio_clock(&d)==0); assert(polls==16);assert(!overrides);
 reset();regs[0xb6]=0xdb;assert(gc573_receiver_update_audio_clock(&d)==0);assert(polls==16);assert(overrides==1);assert(regs[0x8a]==2);
 reset();test_n=0;assert(gc573_receiver_update_audio_clock(&d)==-ENODATA);assert(!writes);test_n=6144;
 reset();test_cts=0;assert(gc573_receiver_update_audio_clock(&d)==-ENODATA);test_cts=148500;
 reset();test_tmds=0;assert(gc573_receiver_update_audio_clock(&d)==-ENODATA);test_tmds=148500;
 reset();test_tmds=297000;assert(gc573_receiver_update_audio_clock(&d)==-EOPNOTSUPP);assert(!writes);test_tmds=148500;
 reset();regs[0x19]=0;assert(gc573_receiver_update_audio_clock(&d)==-ENOLINK);assert(!writes);
 reset();regs[0x19]=255;assert(gc573_receiver_update_audio_clock(&d)==-ENOLINK);assert(!writes);
 reset();regs[0xb6]=0xdb;regs[0x8a]=8;update_fail=2;
 assert(gc573_receiver_update_audio_clock(&d)==-EIO);assert(regs[0x81]==0);assert(regs[0x8a]==8);
 reset();regs[0xb6]=0xdb;regs[0x8a]=8;reset_error=-EIO;update_fail=3;
 assert(gc573_receiver_update_audio_clock(&d)==-EIO);assert(regs[0x8a]==8);reset_error=0;
 puts("clock sequence: match, stable mismatch, invalid/non48k, lost link, partial rollback passed");
}
''')

    def test_ncts_cleanup(self):
        self.compile_run(PRELUDE + function('gc573_it6805_read_ncts') + r'''
int main(void){struct gc573_device d={0};u32 n,c;
 for(int f=2;f<=7;f++) {reset();fail_read=f;assert(gc573_it6805_read_ncts(&d,&n,&c)==-EIO);assert(bank==0);assert((regs[0x86]&1)==0);}
 reset();bank_fail=1;assert(gc573_it6805_read_ncts(&d,&n,&c)==-EIO);assert(updates==1);
 reset();regs[0xbe]=2;regs[0xbf]=0x58;regs[0xc0]=0xd0;regs[0xc1]=0xe2;regs[0xc2]=0x97;
 assert(gc573_it6805_read_ncts(&d,&n,&c)==0);assert(n==9600 && c==928125);
 reset();regs[0x86]=1;assert(gc573_it6805_read_ncts(&d,&n,&c)==0);assert(regs[0x86]==1);
 puts("N/CTS read faults and unknown-bank cleanup passed");
}
''')

class PrepareTests(unittest.TestCase):
    def test_bank_failures(self):
        prefix = PRELUDE.replace('struct gc573_device { int unused; };','struct gc573_device { int lock; };').replace('if (bank_fail) return -EIO;', 'if (bank_fail) { bank=-1; return -EIO; }')
        helpers = r"""
#define GC573_IT6805_BANK_1 1
#define mutex_lock(x) ((void)(x))
#define mutex_unlock(x) ((void)(x))
#define msleep(x) ((void)(x))
struct gc573_audio_status { bool video_scdt,receiver_reports_48k_lpcm_stereo;u8 receiver_scdt_19,infoframe_b2; };
static int clock_error;
static int gc573_receiver_audio_snapshot(struct gc573_device *d,struct gc573_audio_status *s) {
 (void)d;s->video_scdt=true;s->receiver_scdt_19=0xbe;s->receiver_reports_48k_lpcm_stereo=true;s->infoframe_b2=0;return 0;
}
static int gc573_it6805_reset_audio_logic(struct gc573_device *d){(void)d;return 0;}
static int gc573_receiver_update_audio_clock(struct gc573_device *d){(void)d;if(clock_error)bank_fail=1;return clock_error;}
static int gc573_it6805_write(struct gc573_device *d,u8 r,u8 v){(void)d;assert(bank==1);regs[r]=v;writes++;return 0;}
"""
        main = r"""
int main(void){struct gc573_device d={0};
 reset();assert(gc573_receiver_prepare_audio(&d)==0);assert(bank==0);
 reset();bank_fail=1;assert(gc573_receiver_prepare_audio(&d)==-EIO);assert(writes==0);
 reset();clock_error=-EIO;assert(gc573_receiver_prepare_audio(&d)==-EIO);assert(regs[0xc7]==0);
}
"""
        SequenceTests().compile_run(prefix+helpers+function('gc573_receiver_prepare_audio')+main)

class TransportTests(unittest.TestCase):
    def test_queue_discontinuity(self):
        source = (ROOT/'src/gc573_audio_dma.c').read_text()
        start = source.index('static void gc573_audio_dma_work(')
        end = source.index('\nint gc573_audio_dma_init', start)
        prefix = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stddef.h>
#define GC573_AUDIO_BLOCK_BYTES 4
#define GC573_AUDIO_QUEUE_DEPTH 2
#define spin_lock_irqsave(l,f) ((void)(l),(f)=0)
#define spin_unlock_irqrestore(l,f) ((void)(l),(void)(f))
#define container_of(p,t,m) ((t*)((char*)(p)-offsetof(t,m)))
struct work_struct { int unused; };
struct gc573_audio_dma_stats { bool running; };
struct gc573_audio_dma {
 struct work_struct work; int lock; unsigned queued,tail;
 bool running,discontinuity; uint8_t *scratch,*queue;
 void (*push)(void*,const uint8_t*,size_t); void *context;
 unsigned bytes_delivered;
};
static int xruns,frames;
static void push(void *c,const uint8_t *d,size_t n){(void)c;if(!d){assert(!n);xruns++;}else frames++;}
"""
        main = r"""
int main(void) {
 uint8_t q[8]={0},s[4];
 struct gc573_audio_dma a={.running=true,.queued=2,.discontinuity=true,.scratch=s,.queue=q,.push=push};
 gc573_audio_dma_work(&a.work);assert(xruns==1 && frames==0 && !a.running && !a.queued);
 gc573_audio_dma_work(&a.work);assert(xruns==1);
 a.running=true;a.queued=2;gc573_audio_dma_work(&a.work);assert(frames==2 && a.bytes_delivered==8);
 a.running=false;a.discontinuity=true;gc573_audio_dma_work(&a.work);assert(xruns==1);
}
"""
        SequenceTests().compile_run(prefix+source[start:end]+main)

if __name__ == '__main__':
    unittest.main()
