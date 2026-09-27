# Install with DKMS (Ubuntu 24.04 / Linux Mint 22.x)

**Hardware target: the original AVerMedia Live Gamer 4K GC573, PCI ID
`1461:0054`. This release does not support the Live Gamer 4K 2.1 or other
AVerMedia models.**

This is a beta driver for AVerMedia GC573 PCI device `1461:0054`.
It takes ownership of the card when loaded and can interrupt its HDMI/video
path. Stop capture applications and identify the current PCI driver before
loading it. This guide targets Ubuntu 24.04 LTS and Linux Mint 22.x based on
Ubuntu Noble; other distributions are not qualified by this guide.

The current source tree is not a stable release. Read [the project status](../README.md)
and [troubleshooting](TROUBLESHOOTING.md) before installing.

## Inspect the host

Run the read-only helper from the repository root:

```sh
./tools/device-info.sh
```

It reports the running kernel, matching GC573 PCI functions, bound driver,
video nodes associated with each function, and Secure Boot status when
`mokutil` is installed. It makes no device or system changes and does not
require `sudo`.

## Install build dependencies

```sh
sudo apt update
sudo apt install build-essential dkms linux-headers-$(uname -r) pciutils v4l-utils mokutil
```

`pciutils`, `v4l-utils`, and `mokutil` support diagnostics; the module build
requires `build-essential`, DKMS, and headers matching the target kernel.
Check that `/lib/modules/$(uname -r)/build` exists before continuing.

## Install the beta packages (recommended)

Download the matching driver and amd64 control-app packages, plus `SHA256SUMS`,
from the beta prerelease assets when published. Beta1 is tested on Mint 22.3
amd64; Ubuntu 24.04 is an intended, untested target. Do not mix package versions.
In the directory containing the two packages:

```sh
sha256sum --ignore-missing -c SHA256SUMS
sudo apt install ./openlivegamer4k-dkms_0.1.0~beta1_all.deb ./openlivegamer4k-control_0.1.0~beta1_amd64.deb
```

Install matching kernel headers first as above. Inspect the checksum results:
both downloaded packages must say OK. Checksums detect corruption; this release
is not signed. DKMS compiles the driver locally, so the driver package is source,
not a precompiled kernel module. The control app is a compiled amd64 binary.

**First activation is required.** Continue at **Load the module explicitly**
below after installation; installation alone does not enable capture. Do not
also run the source registration steps below if you installed the package.
Launch `openlivegamer4k-control` for status; enabling HDMI audio reloads the idle
driver. Close capture clients first, then follow [OBS setup](OBS.md).

To remove the packaged installation, stop capture clients, unload
`gc573_pure`, then use `sudo apt remove openlivegamer4k-control openlivegamer4k-dkms`.
The manual `dkms remove` instructions at the end are for a source installation.

## Register and build the source with DKMS

The repository includes `dkms.conf` for module `gc573_pure`, version
`0.1.0~beta1`. From the repository root, copy only the source build inputs
into DKMS's source directory. This excludes generated `.mod.c`, `.o`, and
`.ko` files:

```sh
MODULE=gc573_pure
VERSION='0.1.0~beta1'
SOURCE_DIR="/usr/src/${MODULE}-${VERSION}"

sudo install -d "$SOURCE_DIR"
sudo install -m 0644 dkms.conf LICENSE THIRD_PARTY_NOTICES.md "$SOURCE_DIR/"
sudo env SOURCE_DIR="$SOURCE_DIR" find src -type f \
  \( -name '*.c' ! -name '*.mod.c' -o -name '*.h' -o -name Makefile \) \
  -exec sh -c 'for file do install -m 0644 -D "$file" "$SOURCE_DIR/$file"; done' sh {} +
sudo dkms add -m "$MODULE" -v "$VERSION"
sudo dkms build -m "$MODULE" -v "$VERSION" -k "$(uname -r)"
sudo dkms install -m "$MODULE" -v "$VERSION" -k "$(uname -r)"
dkms status "$MODULE/$VERSION"
```

These commands build and install the module for the running kernel. The DKMS
config sets `AUTOINSTALL="yes"`, so DKMS also rebuilds/installs it for kernels
handled by the DKMS autoinstaller after kernel updates. This DKMS rebuild
setting does not load the module. The project creates no
`/etc/modules-load.d/` entry, modprobe options file, or blacklist. The PCI
device table provides a normal kernel modalias, so standard modalias handling
may load the installed driver when the PCI device is probed; use the device
helper to check its actual binding.

## Build without DKMS

For a one-off build that does not install the module or arrange rebuilds after
kernel updates:

```sh
make -C src
```

To build against a different prepared kernel tree, set `KDIR`:

```sh
make -C src KDIR=/path/to/kernel/build
```

The output is `src/gc573_pure.ko`. This path does not install or load it.

## Secure Boot

Check the current state with `mokutil --sb-state`. When Secure Boot is enabled,
Ubuntu's DKMS flow signs locally built modules using a Machine Owner Key (MOK).
If DKMS asks to create/enroll a key, set the one-time enrollment password,
reboot, choose **Enroll MOK** in MokManager, confirm the key, then reboot.
After enrollment, verify the module signer with:

```sh
modinfo -F signer gc573_pure
```

Do not assume DKMS signing is ready merely because the build succeeded; a key
that has not been enrolled will be rejected at load time. Follow Ubuntu's
[Secure Boot documentation](https://documentation.ubuntu.com/security/security-features/platform-protections/secure-boot/)
for the system's exact MOK prompts and policy. Linux Mint follows Ubuntu's
base for this workflow, but details can vary with installed shim/kernel
packages. Avoid disabling Secure Boot as a routine workaround.

## Load the module explicitly

Before loading, stop applications using the card and record its current driver
and PCI BDF from `./tools/device-info.sh`. Resolve any existing driver handoff
carefully; do not blacklist or uninstall another driver. The current source
configuration used by the existing `insmod` instructions is:

```sh
sudo modprobe gc573_pure bridge_link=1 bridge_bootstrap=1 reset_gpio_mask=288 preserve_bridge=0 preserve_receiver=0 test_hpd_after_ms=0
```

The command passes the same options as the source snapshot's `insmod` form.
Parameters supplied to `modprobe` apply only when the module is first loaded.
If PCI modalias handling already loaded `gc573_pure`, a later command with
options will not change that instance. Inspect its live values:

```sh
for parameter in bridge_link bridge_bootstrap reset_gpio_mask preserve_bridge preserve_receiver test_hpd_after_ms; do
    printf '%s=' "$parameter"
    cat "/sys/module/gc573_pure/parameters/$parameter"
done
```

If needed, stop capture users, unload with `sudo modprobe -r gc573_pure`, then
run the explicit `modprobe` command above. Check `dmesg`, `dkms status`, and
`./tools/device-info.sh`; then inspect V4L2
capabilities with `v4l2-ctl --list-devices` and
`v4l2-ctl --list-formats-ext -d /dev/videoN` using the reported node.

## Remove or roll back

Stop capture applications and unload the module before removing the DKMS
entry:

```sh
sudo modprobe -r gc573_pure
sudo dkms remove -m gc573_pure -v '0.1.0~beta1' --all
sudo rm -rf /usr/src/gc573_pure-0.1.0~beta1
```

If the card previously used another driver, use the recorded PCI BDF and
host-specific driver-binding procedure to restore it. Verify the bound driver
with `./tools/device-info.sh`. DKMS removal does not restore a previous
driver binding automatically. Do not remove or alter another driver's files
or configuration as part of rollback.

For a failed load, first inspect `dmesg` and the troubleshooting guide. If the
module is still loaded, stop users and unload it with `sudo modprobe -r
gc573_pure` before attempting another driver handoff.
