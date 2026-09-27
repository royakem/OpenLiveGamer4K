# Report a capture problem

Useful reports let us reproduce and fix failures. The supported card is the
original **GC573, PCI 1461:0054**. Do not include serial numbers or access details.

## From the control app

1. Reproduce the issue. Leave the input connected and, if possible, leave the
   failing capture running so its state and counters are still available.
2. Click **Save diagnostic report…**. The passive collector does not stop or
   start capture, change module settings, save media, or upload anything.
3. Open and review the saved JSON. It contains distro/session, kernel, installed
   package/source revisions, loaded module settings, receiver audio status and
   DMA counters. Missing fields are useful evidence too. Host/user identity,
   home paths, network details, serials and other sound-device names are excluded;
   inspect custom kernel/module strings and your own attachments before sharing.
4. Click **Report a problem on GitHub…**. Fill out the guided form and paste the
   reviewed JSON or attach it in the reproduction field. Include:
   - source device, input resolution/refresh, RGB/YCbCr and HDR state;
   - source audio encoding/rate (supported: stereo 48 kHz LPCM);
   - output format/size/fps and OBS/application version;
   - exact steps, expected/actual behavior, frequency and any workaround.
5. If useful, save a second report after a recovery action and describe what
   changed. Do not reinstall/reload before collecting the failing state.

The repo is private during preparation, so only invited users can file issues.
Once public, the same issue link works for community reports.

## Without the app window

After installing the control package, run as your normal user:

```sh
/usr/libexec/openlivegamer4k/collect-audio-report ./openlivegamer4k-diagnostics.json
```

The file must not exist already. Reports are created with owner-only permissions.
From a source checkout, use `python3 tools/collect-audio-report.py ./report.json`.
If collection fails, report the error and distro/kernel/package versions manually.
Never post an environment dump, SSH settings, full system journal or private media.

For driver-loading errors, a reviewed kernel excerpt can help:

```sh
sudo journalctl -k -b -o cat --no-pager | grep -i gc573
```

Review the output before attaching it. This is optional and is not collected
automatically by the app. Report files describe a snapshot, not a capture-quality
or synchronization measurement.

## Audio troubleshooting and temporary opt-out

Audio is on by default in beta2. An older explicit `audio_experimental=0` option
still takes precedence. The legacy parameter name is retained for compatibility.

- Verify the source sends stereo 48 kHz LPCM through HDMI.
- In OBS add the GC573 ALSA source separately from its V4L2 video source.
- Close other audio clients before using **Run 5-second HDMI test**. Silence
  or a busy result is not proof that the hardware is broken.
- Video stop or HDMI recovery can interrupt audio. Restart the audio source if
  necessary, and record whether that restores it.
- Save diagnostics first, close capture clients, then use **Disable HDMI audio**
  for isolation. The app toggle changes the current load, not saved boot options.

If the driver cannot bind and the app cannot switch audio, stop capture clients
and reload with the documented board options plus the disable override:

```sh
sudo modprobe -r gc573_pure
sudo modprobe gc573_pure bridge_link=1 bridge_bootstrap=1 reset_gpio_mask=288 preserve_bridge=0 preserve_receiver=0 test_hpd_after_ms=0 audio_experimental=0
```

For a persistent opt-out, add `options gc573_pure audio_experimental=0` to your
own modprobe configuration, preserving required bridge options and resolving
conflicting entries. Remove that override to return to the default on the next
load. Do not force-unload a busy driver. Report whether disabling audio restores
video so we can distinguish initialization, source-format and capture problems.
