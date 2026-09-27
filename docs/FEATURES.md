# Features and validation status

**0.1.0-beta2** combines the source-only video driver, working stereo HDMI audio,
and GTK4 control app. It retains the tested capture paths, enables audio by default and adds guided diagnostics.
The maintainer confirmed both picture and music end to end in OBS on Mint 22.3
with kernel 6.17.0-22. Short automated stereo-tone and moving-video tests also
passed. Earlier format/timing measurements below retain their original scope;
they do not establish all combinations or long-duration reliability.

See [release notes](RELEASE-NOTES.md) and [current validation](VALIDATION.md).

## Current source matrix

| Area | Current source behavior | Qualification |
| --- | --- | --- |
| HDMI input pixel format | RGB8 only | 4K60 RGB8 input tested on this snapshot; other timings need regression coverage |
| Input timings in the mode table | 1280×720p at 24, 25, 30, 50, 60; 1920×1080p and 3840×2160p at 24, 25, 30, 50, 60; two exact 1280×800p60 DMT timings | Table entries and EDID capability do not establish input support; each exact timing needs hardware testing |
| V4L2 capture sizes | 3840×2160, 1920×1080, 1280×720, 1280×800 | All four sizes passed color-bar checks from 4K60 input |
| V4L2 frame intervals | 60, 60000/1001 (~59.94), 50, 30, 30000/1001 (~29.97), 25, 24, 24000/1001 (~23.976) fps | Advertised intervals; exact output cadence is not established for every input/rate pair |
| V4L2 formats | YUYV, NV12, RGB24, BGR24 | All four formats passed the color checks below and 300-frame 4K60 runs; long-duration qualification pending |

The timing table is intentionally narrower than the timings an HDMI source or
EDID may advertise. Interlaced modes, high-refresh modes, other geometries, and
unlisted timings must not be inferred as supported. Although 720p24/25/30 are
in the timing table, the current EDID capability check does not accept them.

Format details: YUYV is packed YUY2 byte order. NV12 uses the source hardware
selector `0x0f`; BGR24 uses selector `0x02`. RGB24 is marked emulated and
produced by an in-place red/blue swap in `buf_finish`, which costs CPU time per
buffer. The conversion does not run in the IRQ handler. Prefer BGR24 when an
application supports it and lower CPU cost matters. The smoke test confirms
frame return and expected byte counts for NV12 and RGB/BGR, with a coherent
neutral BGR pattern observed after Night Light was disabled. The seven-bar
analyzer passed. This limited color smoke test is not complete layout or
color qualification. Longer operation remains to be qualified.

### Audio and control app

| Feature | Implemented behavior | Evidence / limit |
| --- | --- | --- |
| HDMI audio | Stereo 48 kHz S16_LE LPCM through ALSA, on by default | Known left/right tones and maintainer music confirmation |
| Concurrent A/V | V4L2 video and ALSA audio can run together | Both startup orders and short OBS recordings passed; long-term drift unqualified |
| Audio recovery | Shared video reset reports an audio XRUN | Client may need audio capture restarted |
| GTK4 control | Status, exclusive preview, audio mode, activity test, report export | X11 hardware tested; native Wayland hardware preview pending |
| Privacy | No automatic report upload; GUI runs unprivileged | Privileged idle-driver reload uses polkit |
| Packaging | DKMS source .deb and amd64 control .deb | Mint 22.3 tested; complete lifecycle and broader distro checks pending |

## Exact-snapshot evidence

On Linux Mint 22.3 with kernel `6.17.0-22-generic`, snapshot
`50bcea63c8c4e1cb37284c79699c946dcb1252a7` built successfully. Three helper
checks and eight analyzer tests passed. Isolated DKMS add, build, and signing
steps also passed on this system. Package installation and MOK enrollment/use
have not been tested; these steps do not establish a working end-to-end DKMS
installation.

With a 4K60 input and 720p output, four frames were returned for each of YUYV,
NV12, RGB24, and BGR24 (16 frames total), with expected byte counts and a
coherent neutral BGR pattern. The seven-bar analyzer passed. The earlier warm
cast, including the measured white RGB `(250, 193, 132)`, resolved when desktop
Night Light was disabled. Color has passed this limited neutral-pattern smoke
check; broader input, layout and long-duration qualification remain pending.

The additional matrix passed: two frames per format at 3840×2160,
1920×1080 and 1280×800, alongside the four frames per format at 1280×720.
That is 40 analyzed frames across 16 format/size combinations, all with a
4K60 input. Each format also completed a 300-frame native 4K run reporting
60 fps (1,200 frames total). Those throughput runs discarded pixel data and
do not prove every frame's image quality or long-duration reliability.
See [VALIDATION.md](VALIDATION.md) for details.

A same-process NV12 capture also completed 2,100 frames through three input
mode changes (4K60 → 1080p50 → 720p60 → 4K60) with automatic recovery.
This verifies delivery recovery, not pixel quality or output cadence during
those transitions. OBS and other format recovery tests remain pending.

## Earlier hardware results (prior source revisions)

Tests in the private development history on one GC573 setup demonstrated:

- Moving YUYV capture at selected 4K, 1080p, and 720p output sizes, including
  source FPGA scaling. Several captures and mode sweeps completed without
  known black frames.
- Selected RGB8 4K and 1080p inputs at 60 and 59.94 Hz, plus tested 720p
  50/60 Hz families and other timing families in an expanded matrix. These
  results do not cover every input/output combination in the source table.
- Selected output cadence checks at 60, 50, 30, 29.97, 25, 24, and 23.976 fps.
  With a 60 Hz input, requesting 59.94 fps measured 60 fps; exact 60-to-59.94
  conversion remains unresolved.
- Input-loss and mode-change recovery with OBS in a controlled setup. Observed
  recovery took approximately 3–11 seconds, depending on the change. It was
  not seamless and was not a general reliability qualification.
- 1280×800 output and selected scaler paths were exercised, but this is not a
  claim that all 1280×800 timings or format combinations work.

These are historical, hardware-specific results from earlier revisions. They
do not qualify the exact snapshot's image quality or long-term behavior. The
[release notes](RELEASE-NOTES.md) tracks remaining work.

## Not implemented or not established

- Audio recovery and long-term synchronization remain unqualified. Video stop
  or HDMI recovery can interrupt the working audio stream with an XRUN.
- P010 and HDR are planned; neither is implemented or qualified.
- HDMI input is RGB8 only; other input encodings and bit depths are not
  supported or qualified.
- Full EDID coverage, every mode/format/rate combination, exact fractional
  cadence, long-duration reliability, cold-start consistency, and broad
  device/kernel compatibility are not established.
