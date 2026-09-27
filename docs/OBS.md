# OBS Studio on Linux

OBS can access the GC573 through its standard **Video Capture Device (V4L2)**
source. This describes the generic Linux V4L2 workflow; it is not a claim that
every OBS package, distribution, kernel, or capture mode has been tested. The
current snapshot returned short frames with expected byte counts in four
formats, and a neutral-pattern smoke check passed after desktop Night Light was
disabled. This is not complete layout or color qualification and does not
establish OBS operation. See the [feature matrix](FEATURES.md) and
[installation notes](INSTALL.md).

## Before opening OBS

1. Build and load the module using [INSTALL.md](INSTALL.md). Confirm that the
   driver created a video device, usually `/dev/video0` (the number may differ).
2. Ensure your account can access that device. Distribution device permissions
   vary; log out and back in if you change group membership.
3. Check what this driver advertises:

   ```sh
   v4l2-ctl --list-devices
   v4l2-ctl -d /dev/video0 --list-formats-ext
   ```

   Replace `/dev/video0` with the GC573 node. The source currently advertises
   YUYV, NV12, RGB24, and BGR24, several sizes, and several frame intervals;
   advertisement does not mean that each option has passed hardware tests.
   Start with YUYV as a conservative format choice. Earlier YUYV results
   predate this snapshot; the current short smoke test returned frames in all
   four formats and passed a neutral-pattern analyzer check, but did not
   completely qualify layout or image quality.

## Add a capture source

1. In OBS, add a **Video Capture Device (V4L2)** source to a scene.
2. Select the GC573 `/dev/videoN` device.
3. Choose **Custom** resolution and select a listed size, such as 3840×2160,
   1920×1080, or 1280×720. Use a frame rate the device lists; begin with 60 fps
   for a 60 Hz source.
4. Select YUYV if the OBS device properties expose a pixel-format option. If
   the format is hidden or set automatically, check the source properties and
   OBS log for the negotiated format.
5. Apply the settings and confirm that moving video appears. If the source
   stays black or fails to start, stop it, verify signal/lock and the selected
   node, then try a listed YUYV size and rate.

Do not use frame delivery or expected byte counts alone to judge image
correctness. A neutral-pattern smoke test passed with Night Light disabled; the
warm cast seen earlier was resolved by disabling it. Check OBS output against
a known reference. Broader color and layout qualification remains pending.

OBS setting names and available controls can vary with its version and the
V4L2 plugin/package. If OBS cannot open the device, close other capture
applications first; V4L2 device access may be exclusive.

## Format and performance notes

YUYV is the conservative starting point. A 4K60-input to 720p-output smoke
test on the current snapshot returned four frames each in YUYV, NV12, RGB24,
and BGR24, with expected byte counts. With Night Light disabled, the neutral
pattern and seven-bar analyzer passed. This limited result is not complete
layout or color qualification and does not establish sustained capture or OBS
operation. Additional output-size and sustained-capture tests are in progress.
RGB24 is emulated with an in-place red/blue swap on completed buffers and
therefore uses extra CPU; BGR24 uses the native hardware selector and may be
preferable when OBS accepts it. Performance under OBS remains untested.

Use the V4L2 source for video and a separate ALSA input for HDMI audio (see
below). 59.94 fps is exposed as an option, but prior testing with a 60 Hz input
measured 60 fps when 59.94 was requested. Earlier timing-transition tests do
not qualify every mode on the current build.

Ubuntu and Linux Mint are intended installation targets, but OBS behavior has
not been validated across their versions or packages. Consult the
[installation guide](INSTALL.md), [feature matrix](FEATURES.md), and
[release notes](RELEASE-NOTES.md) for current scope.

## HDMI audio with concurrent video

Beta2 enables HDMI audio by default on driver load. If an earlier configuration
explicitly disables it, enable audio while capture applications are idle.
Use the normal V4L2 video source and a separate **Audio Capture Device
(ALSA)** source. Select the GC573 endpoint identified by its PCI ancestry; do
not assume a fixed ALSA card number or select a default microphone. This board
currently exposes `hw:CARD=Audio,DEV=0`; verify identity before reusing that name.
Set 48 kHz stereo. Leave monitoring off to avoid feedback unless deliberately needed.

Both source startup orders passed short tests. Video stop or HDMI recovery can
interrupt audio with an XRUN; clients may recover automatically or need restart.
Combined recording has been checked for correct test-tone channels and moving
video, not calibrated A/V offset or long-term drift. See
[validation record](VALIDATION.md).
