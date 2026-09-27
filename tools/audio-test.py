#!/usr/bin/python3
# SPDX-License-Identifier: GPL-2.0-only
"""Bounded, unprivileged five-second GC573 ALSA capture diagnostic."""
import json
import re
import subprocess
import sys

ARECORD = "/usr/bin/arecord"
TIMEOUT = 8
FRAME_BYTES = 4
FRAMES = 48000 * 5


def _cards(root="/sys/class/sound"):
    from pathlib import Path
    matches = []
    for card in Path(root).glob("card[0-9]*"):
        try:
            device = (card / "device").resolve(strict=True)
            pci = device
            while pci != pci.parent and not (pci / "vendor").exists():
                pci = pci.parent
            vendor = (pci / "vendor").read_text().strip().lower().removeprefix("0x")
            device_id = (pci / "device").read_text().strip().lower().removeprefix("0x")
            driver = (pci / "driver").resolve(strict=True)
            if vendor == "1461" and device_id == "0054" and driver.name == "gc573_pure":
                matches.append((card.name[4:], card, pci))
        except OSError:
            continue
    return matches


def _capture(card, *, run=subprocess.run, arecord=ARECORD, timeout=TIMEOUT, sysfs="/sys/module/gc573_pure/parameters/audio_experimental"):
    report = {"ok": False, "result": "inconclusive", "frames": 0, "peak": 0}
    try:
        from pathlib import Path
        value = Path(sysfs).read_text().strip()
    except OSError:
        value = ""
    if value not in ("Y", "1"):
        report.update(result="unsupported", error="HDMI audio is not enabled")
        return report
    cmd = [arecord, "-q", "-D", f"hw:{card},0", "-t", "raw", "-f", "S16_LE", "-r", "48000", "-c", "2", "--period-size=960", "--buffer-size=7680", "-d", "5"]
    try:
        proc = run(cmd, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                   timeout=timeout, env={"PATH":"/usr/bin:/bin", "LANG":"C"}, check=False)
    except subprocess.TimeoutExpired:
        report.update(result="inconclusive", error="capture timed out")
        return report
    except OSError as exc:
        report.update(result="unsupported", error=str(exc))
        return report
    raw = proc.stdout or b""
    frames = len(raw) // FRAME_BYTES
    samples = (int.from_bytes(raw[i:i+2], "little", signed=True) for i in range(0, len(raw)-1, 2))
    peak = max((abs(v) for v in samples), default=0)
    report.update(frames=frames, peak=peak)
    if proc.returncode:
        msg = (proc.stderr or b"").decode("utf-8", "replace") if isinstance(proc.stderr, bytes) else (proc.stderr or "")
        report.update(result="busy" if re.search(r"busy|resource temporarily unavailable", msg, re.I) else "unsupported", error=msg.strip() or "arecord failed")
    elif len(raw) != FRAMES * FRAME_BYTES:
        report.update(result="inconclusive", error="capture returned an unexpected frame count")
    else:
        report.update(ok=True, result="silent" if peak == 0 else "signal")
    return report


def main():
    cards = _cards()
    if len(cards) != 1:
        result = {"ok": False, "result": "unsupported", "error": "expected exactly one matching GC573 ALSA card"}
    else:
        result = _capture(cards[0][0])
    print(json.dumps(result))
    return 0 if result["ok"] else 1

if __name__ == "__main__":
    raise SystemExit(main())
