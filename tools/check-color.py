#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Offline checks for AVI RGB quantization resolution."""
import pathlib
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]

def main():
    with tempfile.TemporaryDirectory() as td:
        d = pathlib.Path(td)
        (d / "linux").mkdir()
        (d / "linux/errno.h").write_text("#define EINVAL 22\n")
        (d / "linux/types.h").write_text("#include <stdint.h>\ntypedef uint8_t u8;\n")
        c = d / "check.c"
        c.write_text(r'''
#include <stdbool.h>
#include <errno.h>
#include "gc573_color.h"
int main(void) {
 bool limited = false;
 unsigned q;
 if (gc573_rgb_quantization_resolve(0, 1, &limited) || limited) return 1;
 if (gc573_rgb_quantization_resolve(0, 0, &limited) || limited) return 2;
 if (gc573_rgb_quantization_resolve(0, 93, &limited) || !limited) return 3;
 if (gc573_rgb_quantization_resolve(0, 94, &limited) || !limited) return 4;
 if (gc573_rgb_quantization_resolve(0, 95, &limited) || !limited) return 5;
 if (gc573_rgb_quantization_resolve(0, 16, &limited) || !limited) return 6;
 for (q = 0; q < 4; q++) {
  int ret = gc573_rgb_quantization_resolve(q, 94, &limited);
  if (q == 1 && (ret || !limited)) return 7;
  if (q == 2 && (ret || limited)) return 8;
  if (q == 3 && ret != -EINVAL) return 9;
 }
 if (gc573_rgb_quantization_resolve(1, 0, 0) != -EINVAL) return 10;
 return 0;
}
''')
        exe = d / "check"
        subprocess.run(["cc", "-std=c11", "-Wall", "-Werror", "-I", str(d), "-I", str(ROOT / "src"), str(c), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
    print("color quantization checks passed")

if __name__ == "__main__":
    main()
