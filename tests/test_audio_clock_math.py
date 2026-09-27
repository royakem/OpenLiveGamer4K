import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src"
C_TEST = ROOT / "tests" / "test_audio_clock_math.c"
MATH_HEADER = SRC / "gc573_audio_clock_math.h"


class AudioClockMathTests(unittest.TestCase):
    """Offline test for the shared IT6805 audio clock math.

    Compiles tests/test_audio_clock_math.c, which includes the very same
    src/gc573_audio_clock_math.h the kernel module compiles, so the
    assertions exercise the exact code the driver runs.
    """

    @classmethod
    def setUpClass(cls):
        if not (C_TEST.exists() and MATH_HEADER.exists()):
            raise unittest.SkipTest("audio clock math sources missing")
        compiler = shutil.which("gcc") or shutil.which("cc")
        if not compiler:
            raise unittest.SkipTest("no C compiler available offline")
        cls.compiler = compiler
        cls.tmp = tempfile.TemporaryDirectory(prefix="gc573_audio_clock_")
        cls.binary = Path(cls.tmp.name) / "test_audio_clock_math"
        cls.result = subprocess.run(
            [cls.compiler, "-std=c99", "-Wall", "-Wextra",
             f"-I{SRC}", str(C_TEST), "-o", str(cls.binary)],
            capture_output=True, text=True)

    @classmethod
    def tearDownClass(cls):
        if getattr(cls, "tmp", None):
            cls.tmp.cleanup()

    def test_01_compiles_clean(self):
        if self.result.returncode != 0:
            self.fail(
                "audio clock math test failed to compile:\n"
                f"stdout: {self.result.stdout}\nstderr: {self.result.stderr}")

    def test_02_math_assertions_pass(self):
        if getattr(self, "_ran", False):
            self.skipTest("already run")
        self._ran = True
        run = subprocess.run([str(self.binary)], capture_output=True, text=True)
        combined = (run.stdout + run.stderr).strip()
        if run.returncode != 0 or "ALL PASS" not in combined:
            self.fail(
                "audio clock math assertions failed:\n" + combined)

    def test_03_no_unexpected_failures(self):
        combined = (self.result.stdout + self.result.stderr)
        if self.result.returncode != 0:
            return  # already failed in test_01
        run = subprocess.run([str(self.binary)], capture_output=True, text=True)
        self.assertNotIn("FAIL:", run.stdout + run.stderr)


if __name__ == "__main__":
    unittest.main()
