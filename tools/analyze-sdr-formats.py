#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Analyze raw SDR frames against the seven-bar Windows test pattern."""

import argparse
import json
import sys
from pathlib import Path

FORMATS = ("YUYV", "NV12", "RGB24", "BGR24")
MAX_FRAME_BYTES = 64 * 1024 * 1024
DEFAULT_MAX_FRAMES = 100
MAX_FRAMES = 1000
COLORS = (
    ("white", (255, 255, 255)), ("yellow", (255, 255, 0)),
    ("cyan", (0, 255, 255)), ("green", (0, 128, 0)),
    ("magenta", (255, 0, 255)), ("red", (255, 0, 0)),
    ("blue", (0, 0, 255)),
)


def frame_size(fmt, width, height):
    if fmt not in FORMATS:
        raise ValueError(f"format must be one of {', '.join(FORMATS)}")
    if width <= 0 or height <= 0:
        raise ValueError("width and height must be positive")
    if width < 8 or height < 8:
        raise ValueError("width and height must be at least 8 pixels")
    if fmt in ("YUYV", "NV12") and width % 2:
        raise ValueError(f"{fmt} requires an even width")
    if fmt == "NV12" and height % 2:
        raise ValueError("NV12 requires an even height")
    pixels = width * height
    return pixels * 2 if fmt == "YUYV" else pixels * 3 // 2 if fmt == "NV12" else pixels * 3


def expected_rgb(color):
    r, g, b = color
    return (round(.183 * r + .614 * g + .062 * b + 16),
            round(-.101 * r - .339 * g + .439 * b + 128),
            round(.439 * r - .399 * g - .040 * b + 128))


def sample_patch(frame, fmt, width, height, cx, cy):
    """Return channel means from up to an 8x8 patch (Y/U/V or R/G/B)."""
    patch_width = min(8, max(1, width // 14))
    patch_height = min(8, height)
    x0, y0 = int(cx * width - patch_width / 2), int(cy * height - patch_height / 2)
    x0 = max(0, min(width - patch_width, x0))
    y0 = max(0, min(height - patch_height, y0))
    totals = [0.0, 0.0, 0.0]
    for y in range(y0, y0 + 8):
        for x in range(x0, x0 + 8):
            if fmt == "YUYV":
                base = (y * width + (x & ~1)) * 2
                vals = (frame[base + (0 if x % 2 == 0 else 2)], frame[base + 1], frame[base + 3])
            elif fmt == "NV12":
                yoff = y * width + x
                uv = width * height + (y // 2) * width + (x & ~1)
                vals = (frame[yoff], frame[uv], frame[uv + 1])
            else:
                off = (y * width + x) * 3
                vals = tuple(frame[off:off + 3])
                if fmt == "BGR24":
                    vals = vals[::-1]
            for i, value in enumerate(vals):
                totals[i] += value
    samples = patch_width * patch_height
    return [v / samples for v in totals]


def analyze_frame(frame, fmt, width, height, tolerance=None):
    tolerance = tolerance if tolerance is not None else (20 if fmt in ("RGB24", "BGR24") else 12)
    patches = []
    passed = True
    for index, (name, rgb) in enumerate(COLORS):
        actual = sample_patch(frame, fmt, width, height, (index + .5) / 7, .25)
        expected = (tuple(rgb) if fmt in ("RGB24", "BGR24") else expected_rgb(rgb))
        channel_names = ("r", "g", "b") if fmt in ("RGB24", "BGR24") else ("y", "u", "v")
        errors = [abs(a - e) for a, e in zip(actual, expected)]
        ok = all(error <= tolerance for error in errors)
        passed &= ok
        patches.append({"color": name, "center": {"x": (index + .5) / 7, "y": .25},
                        "mean": dict(zip(channel_names, actual)),
                        "expected": dict(zip(channel_names, expected)), "pass": ok})
    # The lower 35% should be black; text/moving marker can affect a portion.
    black_values = []
    for y in range(int(height * .65), height):
        for x in range(width):
            if fmt == "YUYV":
                v = frame[(y * width + x) * 2]
            elif fmt == "NV12":
                v = frame[y * width + x]
            else:
                off = (y * width + x) * 3
                rgb = frame[off:off + 3]
                v = max(rgb)
            black_values.append(v)
    black_count = sum(v <= (20 if fmt in ("RGB24", "BGR24") else 32) for v in black_values)
    return {"patches": patches, "pass": passed,
            "black_region": {"pixel_count": len(black_values), "black_count": black_count,
                             "black_fraction": black_count / len(black_values)}}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("format", choices=FORMATS)
    parser.add_argument("width", type=int)
    parser.add_argument("height", type=int)
    parser.add_argument("input", type=Path)
    parser.add_argument("--frames", type=int, help=f"analyze at most this many first frames (maximum {MAX_FRAMES}; default {DEFAULT_MAX_FRAMES})")
    parser.add_argument("--png-dir", type=Path, help="export analyzed first/last frame PNGs with ffmpeg")
    args = parser.parse_args(argv)
    try:
        size = frame_size(args.format, args.width, args.height)
        if args.frames is not None and not 1 <= args.frames <= MAX_FRAMES:
            raise ValueError(f"--frames must be between 1 and {MAX_FRAMES}")
        if size > MAX_FRAME_BYTES:
            raise ValueError(f"frame size {size} exceeds the {MAX_FRAME_BYTES}-byte limit")
        file_bytes = args.input.stat().st_size
        if file_bytes == 0 or file_bytes % size:
            raise ValueError(f"input size {file_bytes} is not a nonzero multiple of frame size {size} (truncated frame)")
        total = file_bytes // size
        count = min(total, args.frames or DEFAULT_MAX_FRAMES)
        results = []
        first = last = None
        with args.input.open("rb") as source:
            for i in range(count):
                frame = source.read(size)
                if len(frame) != size:
                    raise ValueError(f"truncated frame {i}")
                if i == 0:
                    first = frame
                last = frame
                result = analyze_frame(frame, args.format, args.width, args.height)
                result["index"] = i
                results.append(result)
        pngs = []
        if args.png_dir:
            import shutil
            import subprocess
            ffmpeg = shutil.which("ffmpeg")
            if not ffmpeg:
                raise ValueError("--png-dir requires ffmpeg on PATH")
            args.png_dir.mkdir(parents=True, exist_ok=True)
            pixfmt = {"YUYV": "yuyv422", "NV12": "nv12", "RGB24": "rgb24", "BGR24": "bgr24"}[args.format]
            for label, frame in (("first", first), ("last", last)):
                path = args.png_dir / f"{label}.png"
                if path.exists():
                    raise ValueError(f"refusing to overwrite {path}")
                proc = subprocess.run([ffmpeg, "-v", "error", "-f", "rawvideo", "-pixel_format", pixfmt,
                    "-video_size", f"{args.width}x{args.height}", "-i", "pipe:0", "-frames:v", "1", str(path)],
                    input=frame, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
                if proc.returncode:
                    raise ValueError(proc.stderr.decode(errors="replace").strip() or "ffmpeg failed")
                pngs.append(str(path))
        passed = all(item["pass"] for item in results)
        json.dump({"input": str(args.input), "format": args.format, "width": args.width,
                   "height": args.height, "frame_bytes": size, "file_bytes": file_bytes,
                   "frame_count": count, "available_frame_count": total, "frames": results,
                   "png_files": pngs, "pass": passed}, sys.stdout, indent=2)
        print()
        return 0 if passed else 1
    except (OSError, ValueError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
