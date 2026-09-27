# OpenLiveGamer4K Control

Native GTK4 (X11/Wayland) control app for the OpenLiveGamer4K
(AVerMedia Live Gamer 4K, GC573) capture driver.

## What it does

- **Read-only status (default):** device path, driver-reported
  capabilities, input, configured format/size/rate. Safe to run while
  OBS or another V4L2 client owns the node — it never allocates
  streaming buffers or changes capture settings in this mode.
- **Preview (explicit action):** the Start preview button switches to
  the exclusive V4L2 node, negotiates the selected format/size/rate,
  and streams to the window. Stopping preview releases the device
  immediately. If the node is already in use, the app reports the
  failure and asks you to stop the other client; it never preempts it.

## Running

    openlivegamer4k-control              # auto-detect /dev/videoN
    openlivegamer4k-control --backend mock   # deterministic in-process device (offline)

The app launches even when no GC573 node is present and reports that the
device is absent. It runs unprivileged, opens no network connections,
collects no telemetry. Only the narrowly scoped audio activation helper
requests administrator authentication through polkit; the GUI stays unprivileged.

## HDMI audio

Audio is off by default. Read the warning and explicitly opt in, then choose
**Enable HDMI audio**. Administrator authentication may be requested.
The app stops its own preview and reloads an idle driver, briefly reconnecting
HDMI. Other capture clients must be closed; they are never stopped for you.
The installed DKMS module must support `audio_experimental`.

**Run 5-second HDMI test** captures only the card's stereo 48 kHz S16_LE PCM,
reports silence or nonzero activity, and keeps no recording. A nonzero result
is not a fidelity or A/V-sync test. Beta1 includes verified stereo-tone capture,
short simultaneous OBS audio/video tests, and user-confirmed music capture.
Video stop or HDMI recovery can cause an audio XRUN; restart capture if needed.
Long-term synchronization remains unqualified.

**Disable HDMI audio** removes the ALSA endpoint by
reloading the idle driver. No persistent boot setting is changed. Installing
or opening the app never enables audio automatically. Diagnostic sharing is
manual; there is no automatic upload.

## Driver

Capture requires the `openlivegamer4k-dkms` driver package to be
installed and the module loaded (see its README.Debian). This control
package installs and starts independently of the driver.

## License

GPL-2.0-only. Linked against the system GTK4 (LGPL-2.1-or-later),
which is GPL-2.0-compatible.
