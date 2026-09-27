#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Compile and run offline checks for the GC573 scatter-gather descriptor builder."""

import pathlib
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]


def main():
    with tempfile.TemporaryDirectory() as temp:
        root = pathlib.Path(temp)
        linux = root / "linux"
        linux.mkdir()
        (linux / "types.h").write_text(r'''#include <stdint.h>
#include <stddef.h>
typedef uint8_t u8; typedef uint32_t u32; typedef uint64_t u64;
typedef uint32_t __le32;
#define __packed __attribute__((packed))
#define U32_MAX UINT32_MAX
#define static_assert _Static_assert
''')
        (linux / "build_bug.h").write_text('#include "types.h"\n')
        (linux / "dma-mapping.h").write_text('#include "types.h"\ntypedef uint64_t dma_addr_t;\n')
        (linux / "byteorder").mkdir()
        (linux / "byteorder/little_endian.h").write_text(r'''#include <stdint.h>
static inline uint32_t cpu_to_le32(uint32_t x) { return x; }
static inline uint32_t lower_32_bits(uint64_t x) { return (uint32_t)x; }
static inline uint32_t upper_32_bits(uint64_t x) { return (uint32_t)(x >> 32); }
''')
        (linux / "errno.h").write_text('''#define EINVAL 22
#define ENOSPC 28
#define ENOENT 2
#define EOVERFLOW 75
''')
        (linux / "overflow.h").write_text(r'''#include <stdbool.h>
#define check_add_overflow(a,b,p) __builtin_add_overflow((a),(b),(p))
''')
        (linux / "string.h").write_text('#include_next <string.h>\n')
        test_c = root / "check.c"
        test_c.write_text(r'''#include <stdint.h>
#include <stddef.h>
#include <errno.h>
#include <string.h>
#include "gc573_desc.h"

static struct gc573_hw_desc table[4];
static struct gc573_desc_list list = { table, 4, 99 };

static int failed_unchanged(const struct gc573_dma_segment *sg, unsigned int n,
                            size_t off, size_t len, int expected)
{
    struct gc573_hw_desc before[4];
    int ret;
    memset(table, 0xa5, sizeof(table));
    memcpy(before, table, sizeof(table));
    list.count = 77;
    ret = gc573_desc_list_build_range(&list, sg, n, off, len);
    return ret == expected && list.count == 0 &&
           memcmp(table, before, sizeof(table)) == 0;
}

int main(void)
{
    struct gc573_dma_segment one[] = {{ UINT64_C(0x123456780), 32 }};
    struct gc573_dma_segment split[] = {
        { UINT64_C(0x100000010), 8 }, { UINT64_C(0x200000020), 12 }
    };
    struct gc573_dma_segment overflow[] = {{ UINT64_MAX - 3, 8 }};
    struct gc573_dma_segment short_sg[] = {{ 0x1000, 8 }};

    memset(table, 0, sizeof(table));
    if (gc573_desc_list_build_range(&list, one, 1, 4, 12) || list.count != 1)
        return 1;
    if (table[0].dma_addr_low != 0x23456784 || table[0].dma_addr_high != 1 ||
        table[0].length_words != 3 || table[0].control != GC573_DESC_CONTROL)
        return 2;

    if (gc573_desc_list_build_range(&list, split, 2, 4, 12) || list.count != 2)
        return 3;
    if (table[0].dma_addr_low != 0x14 || table[0].dma_addr_high != 1) return 41;
    if (table[0].length_words != 1) return 42;
    if (table[1].dma_addr_low != 0x20 || table[1].dma_addr_high != 2) return 43;
    if (table[1].length_words != 2) return 44;

    if (!failed_unchanged(short_sg, 1, 4, 8, -EINVAL)) return 5;
    if (!failed_unchanged(short_sg, 1, 0, 0, -EINVAL)) return 6;
    if (!failed_unchanged(short_sg, 1, 2, 4, -EINVAL)) return 7;
    if (!failed_unchanged(overflow, 1, 0, 4, -EOVERFLOW)) return 8;
    if (!failed_unchanged(short_sg, 1, SIZE_MAX - 3, 8, -EOVERFLOW)) return 9;

    list.capacity = 1;
    if (!failed_unchanged(split, 2, 4, 12, -ENOSPC)) return 10;
    list.capacity = 4;
    if (!failed_unchanged(NULL, 0, 0, 4, -EINVAL)) return 11;
    return 0;
}
''')
        exe = root / "check-descriptor-planes"
        subprocess.run([
            "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
            "-I", str(root), "-I", str(ROOT / "src"),
            str(test_c), str(ROOT / "src/gc573_desc.c"), "-o", str(exe),
        ], check=True)
        subprocess.run([str(exe)], check=True)
    print("descriptor plane range checks passed")


if __name__ == "__main__":
    main()
