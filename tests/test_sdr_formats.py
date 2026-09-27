import importlib.util
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


SCRIPT = Path(__file__).resolve().parents[1] / "tools" / "analyze-sdr-formats.py"
spec = importlib.util.spec_from_file_location("analyze_sdr_formats", SCRIPT)
analyzer = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = analyzer
spec.loader.exec_module(analyzer)

WIDTH, HEIGHT = 112, 80


def synthetic(fmt, swap_uv=False, swap_rgb=False):
    yplane = bytearray(WIDTH * HEIGHT)
    uvplane = bytearray(WIDTH * HEIGHT // 2)
    packed = bytearray()
    for y in range(HEIGHT):
        for x in range(WIDTH):
            bar = min(6, x * 7 // WIDTH)
            rgb = analyzer.COLORS[bar][1]
            yy, uu, vv = analyzer.expected_rgb(rgb)
            if y >= int(HEIGHT * .65):
                yy, uu, vv, rgb = (16, 128, 128, (0, 0, 0))
            if fmt == "YUYV":
                if x % 2 == 0:
                    packed.extend((yy, vv if swap_uv else uu))
                else:
                    packed.extend((vv, yy) if swap_uv else (yy, vv))
            elif fmt == "NV12":
                yplane[y * WIDTH + x] = yy
                if x % 2 == 0 and y % 2 == 0:
                    off = (y // 2) * WIDTH + x
                    uvplane[off:off + 2] = bytes((vv, uu) if swap_uv else (uu, vv))
            else:
                reverse = (fmt == "BGR24") ^ swap_rgb
                channels = rgb[::-1] if reverse else rgb
                packed.extend(channels)
    return bytes(yplane + uvplane if fmt == "NV12" else packed)


class SDRFormatTests(unittest.TestCase):
    def test_green_reference_matches_system_drawing_green(self):
        self.assertEqual(analyzer.COLORS[3], ("green", (0, 128, 0)))

    def test_synthetic_pattern_passes_each_format(self):
        for fmt in analyzer.FORMATS:
            with self.subTest(fmt=fmt):
                report = analyzer.analyze_frame(synthetic(fmt), fmt, WIDTH, HEIGHT)
                self.assertTrue(report["pass"])
                self.assertEqual(len(report["patches"]), 7)

    def test_rgb_channel_swap_is_detected(self):
        report = analyzer.analyze_frame(synthetic("RGB24", swap_rgb=True), "RGB24", WIDTH, HEIGHT)
        self.assertFalse(report["pass"])

    def test_nv12_wrong_uv_order_is_detected(self):
        report = analyzer.analyze_frame(synthetic("NV12", swap_uv=True), "NV12", WIDTH, HEIGHT)
        self.assertFalse(report["pass"])

    def test_yuyv_wrong_uv_order_is_detected(self):
        report = analyzer.analyze_frame(synthetic("YUYV", swap_uv=True), "YUYV", WIDTH, HEIGHT)
        self.assertFalse(report["pass"])

    def test_nv12_uv_plane_starts_after_full_y_plane(self):
        frame = synthetic("NV12")
        off = WIDTH * HEIGHT
        self.assertEqual(frame[off:off + 2], bytes(analyzer.expected_rgb((255, 255, 255))[1:]))

    def test_cli_rejects_partial_frame(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "partial.raw"
            path.write_bytes(synthetic("RGB24")[:-1])
            result = subprocess.run([sys.executable, str(SCRIPT), "RGB24", str(WIDTH), str(HEIGHT), str(path)],
                                    text=True, capture_output=True, check=False)
            self.assertEqual(result.returncode, 2)
            self.assertIn("truncated", result.stderr)

    def test_cli_rejects_frame_over_limit(self):
        result = subprocess.run([sys.executable, str(SCRIPT), "RGB24", "5000", "5000", "unused.raw"],
                                text=True, capture_output=True, check=False)
        self.assertEqual(result.returncode, 2)
        self.assertIn("exceeds", result.stderr)


if __name__ == "__main__":
    unittest.main()
