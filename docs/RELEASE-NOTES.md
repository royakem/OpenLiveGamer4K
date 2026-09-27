# OpenLiveGamer4K 0.1.0-beta2

**Hardware target: the original AVerMedia Live Gamer 4K GC573, PCI ID
`1461:0054`. This release does not support the Live Gamer 4K 2.1 or other
AVerMedia models.**

**Beta prerelease for community testing.** Source-only Linux capture for the
AVerMedia Live Gamer 4K GC573 (`1461:0054`), with working HDMI video and audio.
No vendor driver binary or firmware payload is required by this distribution.

## Included features

- V4L2 video: YUYV/YUY2, NV12, RGB24 and BGR24.
- FPGA output scaling: 3840×2160, 1920×1080, 1280×720 and 1280×800.
- Selected RGB8 SDR input modes through 4K60; input-change recovery implemented.
- Stereo 48 kHz S16_LE LPCM HDMI capture through ALSA, concurrently with video.
- OBS video and audio capture verified with stereo test tones and maintainer
  confirmation of picture and music working end to end.
- GTK4 control app: device status, exclusive video preview, HDMI audio controls,
  a five-second audio activity test, and locally saved diagnostic reports.
- DKMS source package and separate amd64 control-app package.

Beta2 enables HDMI audio by default and adds a GitHub issue link, guided bug
form and diagnostic schema v2 with distribution, package and source revisions.
The video/audio capture engines are unchanged from the tested alpha4 path.

## Downloads

| Asset | Purpose |
| --- | --- |
| `openlivegamer4k-dkms_0.1.0-beta2_all.deb` | Driver source; DKMS compiles for the installed kernel |
| `openlivegamer4k-control_0.1.0-beta2_amd64.deb` | Compiled GTK4 control app and audio helpers |
| `openlivegamer4k-0.1.0-beta2-source.tar.xz` | Corresponding complete source, including Debian packaging |
| `openlivegamer4k-0.1.0-beta2-debian-source.tar.gz` | Debian source bundle with canonical `.dsc` and tarball filenames |
| `SOURCE-REVISION.txt`, `SHA256SUMS` | Exact source commit and artifact checksums |

The optional buildinfo/changes files record build metadata; debug symbols are
not part of the proposed public download set. Artifacts are unsigned.
See [installation](INSTALL.md) for dependencies, checksums, activation and removal.

## Tested setup and first use

Linux Mint 22.3, amd64, kernel 6.17.0-22-generic, X11, one GC573 card.
Ubuntu 24.04 is an intended target, not independently qualified. Other kernel,
distribution and architecture combinations need testing.

Install both matching packages with `apt` and matching kernel headers.
**Installation alone is not capture-ready:** load with the documented bridge
options in [installation](INSTALL.md). Audio is enabled by default on driver load;
an existing explicit disable option remains respected. Add a V4L2
video source and ALSA audio source in [OBS](OBS.md). Changing audio mode reloads
the idle driver and reconnects HDMI; the app does not change boot settings.

## Known limitations and requested community testing

- Video stop or HDMI recovery can interrupt audio with an ALSA XRUN. Restart
  audio capture if the client does not recover. Seamless hotplug is not claimed.
- Sustained A/V synchronization, repeated cold starts, two-hour soak and the
  full native-4K format-duration matrix remain unqualified.
- HDMI audio is stereo 48 kHz LPCM only; other rates and compressed surround
  formats are unsupported. P010/HDR are planned, not implemented.
- HDMI input requires RGB8. Advertised timings are broader than measured
  coverage; independent 60 Hz input to 59.94 fps output conversion is unresolved.
- Native Wayland hardware preview remains unverified; X11 hardware operation
  and nested Wayland mock operation have been checked.
- Fresh-machine package lifecycle, kernel-update, Secure Boot and Ubuntu
  qualification are incomplete. No automatic driver replacement is provided.

Use **Save diagnostic report…**, review it, then **Report a problem on GitHub…**
in the control app. See [the reporting guide](REPORTING.md). Please include the driver/package version, kernel, distribution, input resolution
and refresh rate, output format, OBS version, and reproduction steps. The app's
report stays local until you choose to share it. Review it before posting.
Audio activity detection alone is not a fidelity or synchronization measurement.

GPL-2.0-only. Author/maintainer: Roy-Åke Martinsson. Upstream attribution is in
[THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md).
