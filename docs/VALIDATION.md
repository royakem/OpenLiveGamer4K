# Validation summary — beta2

Beta2 enables audio by default and extends passive issue diagnostics. Existing
capture results below describe the same video/audio engines with audio enabled;
changing the default does not add new hardware coverage. Diagnostic/default
regressions are included in the package build.

## Tested setup

- Original AVerMedia Live Gamer 4K GC573, PCI ID `1461:0054`; one card.
- Linux Mint 22.3, amd64, X11; kernel 6.17.0-22-generic.
- RGB8 SDR HDMI input; tested source and installed module matched.

## Video evidence

All four SDR formats (YUYV, NV12, RGB24 and BGR24) passed selected color-bar
checks at 3840×2160, 1920×1080, 1280×720 and 1280×800 output from 4K60 input.
Native 4K60 passed short 300-frame runs in each format. A later YUYV run
completed 18,000 frames in approximately five minutes. These observations do
not qualify sustained capture in every format or input timing combination.

Input-loss and selected mode changes recovered in earlier OBS testing, taking
approximately 3–11 seconds. Exact independent 60-to-59.94 fps conversion is
unresolved. A displayed frame count alone is not evidence of a live image;
current OBS verification included a visibly advancing test-pattern timer.

## HDMI audio and simultaneous capture

Stereo 48 kHz S16_LE LPCM capture passed left/right tone checks at 997 Hz and
1499 Hz. Both video-first and audio-first startup orders passed short tests.
The maintainer separately confirmed real music and picture working end to end.

One installed-build OBS recording contained 28.928 seconds of decoded stereo
audio, correct separated test tones and concurrent 1280×720 YUYV video at
60 fps. The stop-request statistics reported 1,729 output frames, no encoder
skips and six additional rendering skips during that interval. This does not
establish long-term synchronization, calibrated A/V offset or a drop-free session.
Audio queue-overrun and invalid-selector counters remained zero in that test.

Stopping video or recovering HDMI can cause an audio XRUN. Clients may recover
automatically or require audio capture to be restarted.

## Software and packaging checks

App backend, conversion and worker tests passed. Audio checks covered 11 helper
cases, three clock-math cases, four clock-sequence/fault-injection harnesses,
one shared-reset harness and eight SDR cases. The sequence/reset harnesses
exercise extracted driver functions under ASan/UBSan with simulated I2C faults.
Hardware behavior cannot be established by those offline tests alone.

Clean and repeated Debian source/binary builds passed. The packaging cleanup
removes generated binaries before source packaging. DKMS and the control app
were installed and used on the tested Mint setup. X11 hardware preview and
nested Wayland mock operation were checked; native Wayland hardware preview
remains unqualified.

For each distributed build, SOURCE-REVISION and SHA256SUMS identify the exact
public source and downloads. Internal test recordings, raw hardware research,
access details and agent work logs are deliberately not distributed.

## Remaining qualification

Two-hour soak and A/V drift, full sustained native-4K format coverage, repeated
cold starts and recovery cycles, native Wayland real preview, clean-machine
package lifecycle, kernel updates, Secure Boot and Ubuntu testing remain open.
See [release notes](RELEASE-NOTES.md) for the supported beta scope.
