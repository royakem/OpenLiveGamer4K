#!/usr/bin/python3 -I
# SPDX-License-Identifier: GPL-2.0-only
"""Privileged, narrow GC573 HDMI audio module switch."""
import fcntl
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import stat

PCI_ID = ("1461", "0054")
MODULE = "gc573_pure"
# Values are copied from live sysfs, never accepted from this program's caller.
PARAMS = {
    "trace_frames": "bool", "bridge_tx_only": "bool", "bridge_link": "bool",
    "bridge_bootstrap": "bool", "test_hpd_after_ms": "uint",
    "audio_dma_probe": "bool", "preserve_bridge": "bool",
    "reset_gpio_mask": "uint", "preserve_receiver": "bool",
}
SYS_MODULE = Path("/sys/module/gc573_pure")
SYS_BUS = Path("/sys/bus/pci/devices")
SYS_CLASS = Path("/sys/class")
SYS_SOUND = Path("/sys/class/sound")
DEV_CHAR = Path("/dev/char")
LOCK = Path("/run/lock/openlivegamer4k-audio.lock")
COMMANDS = {"modprobe": "/sbin/modprobe", "modinfo": "/sbin/modinfo", "fuser": "/usr/bin/fuser"}
ENV = {"PATH": "/usr/sbin:/usr/bin:/sbin:/bin", "LANG": "C", "LC_ALL": "C"}

class Failure(Exception):
    pass


def _safe_read(path):
    return Path(path).read_text(encoding="ascii").strip()


def _bool(value):
    if value not in ("Y", "N", "1", "0"):
        raise Failure("invalid boolean module parameter")
    return value


def _uint(value):
    if not re.fullmatch(r"(?:0[xX][0-9a-fA-F]+|[0-9]+)", value):
        raise Failure("invalid numeric module parameter")
    return value


def _devices(bus=None):
    result = []
    for entry in Path(SYS_BUS if bus is None else bus).iterdir():
        try:
            vendor = _safe_read(entry / "vendor").lower().removeprefix("0x")
            device = _safe_read(entry / "device").lower().removeprefix("0x")
        except (OSError, UnicodeError):
            continue
        if (vendor, device) == PCI_ID:
            result.append(entry)
    if len(result) != 1:
        raise Failure("expected exactly one PCI 1461:0054 device")
    driver = (result[0] / "driver").resolve(strict=True)
    if driver.name != MODULE:
        raise Failure("matching PCI device is not bound to gc573_pure")
    return result


def _module_params(root=None):
    root = SYS_MODULE if root is None else Path(root)
    params = []
    for name, kind in PARAMS.items():
        path = Path(root) / "parameters" / name
        try:
            value = _safe_read(path)
        except FileNotFoundError:
            continue
        except OSError as exc:
            raise Failure("cannot read current module options") from exc
        value = _bool(value) if kind == "bool" else _uint(value)
        params.append(f"{name}={value}")
    return params


def _runner(argv, timeout=60):
    return subprocess.run(argv, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE, text=True, timeout=timeout,
                          env=ENV, check=False)


def _loaded():
    return SYS_MODULE.exists()


def _alsa_matches(root=None):
    devices = [item.resolve() for item in _devices()]
    root = SYS_SOUND if root is None else Path(root)
    for card in root.glob("card[0-9]*"):
        try:
            endpoint = (card / "device").resolve(strict=True)
            if any(endpoint == pci or pci in endpoint.parents for pci in devices):
                return True
        except OSError:
            continue
    return False


def _state():
    enabled = _safe_read(SYS_MODULE / "parameters/audio_experimental")
    if enabled not in ("Y", "N", "1", "0"):
        raise Failure("invalid audio_experimental state")
    return enabled in ("Y", "1")


def _busy():
    pci_devices = [item.resolve() for item in _devices()]
    nodes = []
    # Resolve actual character-device nodes reported by sysfs, so fuser checks
    # the same /dev entries opened by V4L2 and ALSA clients.
    for subsystem, pattern in (("video4linux", "video*"), ("sound", "pcmC*D*")):
        for entry in SYS_CLASS.joinpath(subsystem).glob(pattern):
            try:
                endpoint = (entry / "device").resolve(strict=True)
                if not any(endpoint == pci or pci in endpoint.parents for pci in pci_devices):
                    continue
                major_minor = _safe_read(entry / "dev")
                if not re.fullmatch(r"[0-9]+:[0-9]+", major_minor):
                    raise Failure("invalid device node metadata")
                node = DEV_CHAR / major_minor
                if not node.exists():
                    raise Failure("cannot resolve a matching device node")
                nodes.append(str(node))
            except FileNotFoundError:
                continue
            except OSError as exc:
                raise Failure("cannot inspect matching device nodes") from exc
    if not nodes:
        raise Failure("cannot identify PCI device nodes for busy check")
    # Include the card control node as well as every PCM playback/capture node.
    for entry in SYS_SOUND.glob("controlC*"):
        try:
            endpoint = (entry / "device").resolve(strict=True)
            if any(endpoint == pci or pci in endpoint.parents for pci in pci_devices):
                major_minor = _safe_read(entry / "dev")
                if not re.fullmatch(r"[0-9]+:[0-9]+", major_minor):
                    raise Failure("invalid sound device node metadata")
                node = DEV_CHAR / major_minor
                if node.exists():
                    nodes.append(str(node))
        except FileNotFoundError:
            continue
        except OSError as exc:
            raise Failure("cannot inspect matching sound nodes") from exc
    result = _runner([COMMANDS["fuser"], *nodes], timeout=10)
    if result.returncode not in (0, 1) or (result.returncode == 1 and result.stderr.strip()):
        raise Failure("fuser could not check device users")
    if result.returncode == 0 or result.stdout.strip():
        raise Failure("matching video or sound device is busy")


def _verify_installed_parameter():
    result = _runner([COMMANDS["modinfo"], "-p", MODULE], timeout=10)
    if result.returncode != 0:
        raise Failure("cannot inspect installed gc573_pure module parameters")
    declared = {line.split(":", 1)[0].strip() for line in result.stdout.splitlines() if ":" in line}
    if "audio_experimental" not in declared:
        raise Failure("installed gc573_pure does not declare audio_experimental")


def _load(params, enabled):
    args = [COMMANDS["modprobe"], MODULE, *params, "audio_experimental=" + ("1" if enabled else "0")]
    result = _runner(args)
    # Probe success, binding and actual loaded option are independently checked.
    if not _loaded() or not _devices() or _state() != enabled:
        raise Failure("module probe or post-load verification failed")
    if _alsa_matches() != enabled:
        raise Failure("post-load ALSA state does not match requested audio mode")
    if result.returncode != 0:
        raise Failure("modprobe reported failure after load")


def _unload():
    result = _runner([COMMANDS["modprobe"], "-r", MODULE])
    if result.returncode != 0 or _loaded():
        raise Failure("module is in use or failed to unload")


def _lock():
    flags = os.O_CREAT | os.O_RDWR | getattr(os, "O_NOFOLLOW", 0)
    fd = os.open(LOCK, flags, 0o600)
    st = os.fstat(fd)
    if not stat.S_ISREG(st.st_mode) or st.st_uid != 0 or st.st_mode & 0o077:
        os.close(fd)
        raise Failure("unsafe audio-helper lock file")
    try:
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError as exc:
        os.close(fd)
        raise Failure("another audio mode change is in progress") from exc
    return fd


def switch(action, *, run=None, root=None, bus=None, lock_path=None, sound_root=None, class_root=None, dev_char=None):
    global _runner, SYS_MODULE, SYS_BUS, SYS_SOUND, SYS_CLASS, DEV_CHAR, LOCK
    old_runner, old_module, old_bus, old_sound, old_class, old_dev, old_lock = _runner, SYS_MODULE, SYS_BUS, SYS_SOUND, SYS_CLASS, DEV_CHAR, LOCK
    _runner = run or old_runner
    SYS_MODULE = Path(root) if root is not None else old_module
    SYS_BUS = Path(bus) if bus is not None else old_bus
    LOCK = Path(lock_path) if lock_path is not None else old_lock
    SYS_SOUND = Path(sound_root) if sound_root is not None else old_sound
    SYS_CLASS = Path(class_root) if class_root is not None else old_class
    DEV_CHAR = Path(dev_char) if dev_char is not None else old_dev
    fd = None
    try:
        if action not in ("enable", "disable"):
            raise Failure("usage: audio-helper.py enable|disable")
        if os.geteuid() != 0:
            raise Failure("audio-helper must run as root")
        fd = _lock()
        _devices()
        params = _module_params()
        original = _state()
        _verify_installed_parameter()
        desired = action == "enable"
        if original and not _alsa_matches():
            raise Failure("enabled audio mode has no matching GC573 ALSA card")
        if original != desired:
            _busy()
            _unload()
            try:
                _load(params, desired)
            except Exception as exc:
                rollback = "rollback succeeded"
                try:
                    if _loaded():
                        _unload()
                    _load(params, original)
                except Exception as rollback_exc:
                    rollback = "rollback failed: " + str(rollback_exc)
                raise Failure(f"switch failed ({exc}); {rollback}") from exc
        return {"ok": True, "enabled": _state()}
    except Exception as exc:
        return {"ok": False, "error": str(exc)}
    finally:
        if fd is not None:
            os.close(fd)
        _runner, SYS_MODULE, SYS_BUS, SYS_SOUND, SYS_CLASS, DEV_CHAR, LOCK = old_runner, old_module, old_bus, old_sound, old_class, old_dev, old_lock


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    if len(argv) != 1 or argv[0] not in ("enable", "disable"):
        print(json.dumps({"ok": False, "error": "usage: audio-helper enable|disable"}), file=sys.stderr)
        return 2
    result = switch(argv[0])
    if result["ok"]:
        print(json.dumps(result))
        return 0
    print(result["error"], file=sys.stderr)
    return 1

if __name__ == "__main__":
    raise SystemExit(main())
