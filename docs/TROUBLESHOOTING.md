# Troubleshooting

These steps target Ubuntu 24.04 and Linux Mint 22.x based on Ubuntu Noble.
Other distributions and kernel packaging are not qualified.

Start with the read-only inventory:

```sh
./tools/device-info.sh
dkms status gc573_pure/0.1.0~beta2
sudo dmesg -T | tail -n 120
```

`device-info.sh` does not need `sudo`; `dmesg` may require it depending on
kernel log permissions.

## DKMS build fails

Confirm the headers match the kernel being built and the prepared build link
exists:

```sh
uname -r
test -e "/lib/modules/$(uname -r)/build/Makefile" && echo 'headers found'
dpkg -l "linux-headers-$(uname -r)" dkms build-essential
```

Install matching Ubuntu headers if missing, then retry the explicit build:

```sh
sudo apt install "linux-headers-$(uname -r)" dkms build-essential
sudo dkms build -m gc573_pure -v '0.1.0~beta2' -k "$(uname -r)"
```

Inspect `/var/lib/dkms/gc573_pure/0.1.0~beta2/build/make.log` for the first
compiler or kernel API error. `AUTOINSTALL="yes"` asks DKMS to rebuild/install
for kernels processed by its autoinstaller; it does not by itself load the
module. If a kernel update has no matching headers or build fails, that
kernel's DKMS status will show the failure.

## Module load is rejected with a signature or key error

Check Secure Boot and whether the module has a signer:

```sh
 mokutil --sb-state
 modinfo -F signer gc573_pure
 sudo dmesg -T | grep -Ei 'secure boot|verification|signature|key|gc573_pure' | tail -n 80
```

With Secure Boot enabled, Ubuntu's DKMS integration signs third-party modules
using a MOK. If prompted during DKMS setup, enroll the generated key from
MokManager at reboot; a build can succeed while load is still denied because
the signing key is not enrolled. Check the current DKMS logs and Ubuntu's
[Secure Boot documentation](https://documentation.ubuntu.com/security/security-features/platform-protections/secure-boot/)
for the MOK flow supported by the installed shim/kernel packages. Mint uses
an Ubuntu base, while the exact integration depends on its installed packages.

Do not treat disabling Secure Boot as the default fix. If a MOK was not
enrolled, complete its enrollment and reboot, then retry the explicit load.

## GC573 not found or no video node

The helper searches sysfs for PCI vendor/device `1461:0054`, reports the
currently bound driver, and lists video nodes linked to that PCI function.

- If no matching PCI function appears, check the card's seating, firmware
  visibility, and `lspci -Dnn` output.
- If another driver is bound, stop applications using the card and resolve
  the driver handoff using that driver's documented procedure. Do not
  blacklist or remove it as a troubleshooting shortcut.
- If `gc573_pure` is bound but has no video node, inspect `dmesg` for probe or
  V4L2 registration errors and confirm `dkms status` for the running kernel.
- If a node exists, check `v4l2-ctl --list-devices` and
  `v4l2-ctl --all -d /dev/videoN` for the reported device.

If `gc573_pure` was already loaded by PCI modalias handling, parameters on a
later `modprobe gc573_pure name=value` command are not applied to that running
instance. Read live values under `/sys/module/gc573_pure/parameters/`. After
stopping capture users, unload with `sudo modprobe -r gc573_pure`, then load
again with the intended parameters. Do not unload while capture is active.

## Module is busy or removal fails

Stop applications that have the video node open, then inspect users and
unload explicitly:

```sh
sudo fuser -v /dev/videoN
sudo modprobe -r gc573_pure
```

Do not force removal. If another driver's PCI function was previously
unbound, use the recorded BDF and the host-specific binding procedure to
restore it. DKMS removal does not restore PCI binding automatically.

## Kernel update did not produce a usable module

Check the module's DKMS status and the new kernel's build log:

```sh
dkms status gc573_pure/0.1.0~beta2
sudo dkms build -m gc573_pure -v '0.1.0~beta2' -k KERNEL_VERSION
sudo dkms install -m gc573_pure -v '0.1.0~beta2' -k KERNEL_VERSION
```

Replace `KERNEL_VERSION` with the exact version from `uname -r` after booting
that kernel, and make sure its headers are installed. Review the build log
before loading. A successful DKMS install does not imply the driver has been
loaded or qualified on that kernel.

## Warm or incorrect colors

For color-bar tests, temporarily disable Night light, f.lux and other sender
color filters. A sender-side Night light setting produced a warm cast in all
four capture formats during testing; neutral-source checks passed after it
was disabled. Check the sender before attributing a color shift to the driver.
Use RGB8 SDR input; P010/HDR input/output support is not implemented.

## Report a problem

Use the control app to save diagnostics before changing the failing state, then
open its GitHub issue link. See [reporting instructions](REPORTING.md) for required
details, privacy review, and the audio-disable workaround.
