#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Write a passive, redacted GC573 HDMI-audio diagnostics report as JSON."""

import argparse
import json
import gzip
import os
import subprocess
from pathlib import Path
import re
from datetime import datetime, timezone


PCI_VENDOR = "0x1461"
PCI_DEVICE = "0x0054"
PARAMETERS = {"audio_experimental", "audio_dma_probe", "bridge_link", "bridge_bootstrap",
              "bridge_tx_only", "preserve_bridge", "preserve_receiver", "reset_gpio_mask",
              "test_hpd_after_ms", "trace_frames"}
STATUS_FIELDS = set("""experimental_pcm infoframe_b0 infoframe_b1 infoframe_b2
receiver_scdt_19 audio_output_c7 audio_control_81 audio_control_8a audio_control_8c
infoframe_valid video_scdt audio_rate_b5 audio_rate_b6 receiver_rate_code
receiver_48k_lpcm_stereo n_raw cts_raw n_decoded cts_decoded counters_coherent
dma_running dma_irqs bytes_queued bytes_delivered queue_overruns invalid_selector
untouched_blocks nonzero_blocks fpga_audio_detector""".split())



def read_text(path):
    try:
        return path.read_text(encoding="utf-8").strip()
    except (OSError, UnicodeError):
        return None


def parse_status(text):
    values = {}
    if text is None:
        return None
    for line in text.splitlines():
        for match in re.finditer(r"([a-zA-Z0-9_]+)=([^\s]+)", line):
            key, value = match.groups()
            if key in STATUS_FIELDS and re.fullmatch(r"(?:0x)?[0-9a-fA-F]+|-?[0-9]+", value):
                values[key] = value
    return values


def pci_ancestors(path):
    try:
        resolved = path.resolve(strict=True)
    except OSError:
        return set()
    return {part.name for part in (resolved, *resolved.parents)
            if re.fullmatch(r"[0-9a-fA-F]{4}:[0-9a-fA-F]{2}:[0-9a-fA-F]{2}\.[0-7]", part.name)}


def alsa_cards(sys_root, proc_root, pci_addresses):
    cards_dir = sys_root / "class/sound"
    cards = []
    try:
        entries = sorted(cards_dir.glob("card[0-9]*"), key=lambda item: item.name)
    except OSError:
        entries = []
    for entry in entries:
        number = entry.name[4:]
        if not number.isdigit():
            continue
        ancestors = pci_ancestors(entry / "device")
        if not (ancestors & pci_addresses):
            continue
        item = {"number": int(number)}
        card_id = read_text(entry / "id")
        if card_id and re.fullmatch(r"Audio(?:_[0-9]+)?", card_id):
            item["id"] = card_id
        cards.append(item)
    return cards



def system_info(etc_root=Path("/etc")):
    fields = {}
    for line in (read_text(etc_root / "os-release") or "").splitlines():
        key, separator, value = line.partition("=")
        if separator:
            fields[key] = value.strip().strip('"').strip("'")
    known = {"linuxmint", "ubuntu", "debian", "fedora", "arch", "opensuse-leap",
             "opensuse-tumbleweed", "pop", "gentoo", "nixos", "manjaro"}
    distro = fields.get("ID")
    version = fields.get("VERSION_ID", "")
    session = os.environ.get("XDG_SESSION_TYPE")
    return {"distribution": distro if distro in known else "other-or-unknown",
            "version": version if re.fullmatch(r"[0-9][0-9._-]{0,31}", version) else None,
            "session": session if session in {"x11", "wayland", "tty"} else "unknown"}


def software_info(run=subprocess.run, doc_root=Path("/usr/share/doc/openlivegamer4k-dkms")):
    packages = {}
    try:
        result = run(["/usr/bin/dpkg-query", "-W", "-f=${binary:Package}\t${Version}\n",
                      "openlivegamer4k-dkms", "openlivegamer4k-control"],
                     capture_output=True, text=True, timeout=3,
                     env={"PATH": "/usr/bin:/bin", "LANG": "C", "LC_ALL": "C"})
        for line in result.stdout.splitlines():
            name, separator, version = line.partition("\t")
            name = name.split(":", 1)[0]
            if separator and name in {"openlivegamer4k-dkms", "openlivegamer4k-control"}:
                if re.fullmatch(r"[0-9][0-9A-Za-z.+:~_-]{0,63}", version):
                    packages[name] = version
    except (OSError, subprocess.SubprocessError):
        pass
    revision = None
    for name in ("SOURCE-REVISION.md", "SOURCE-REVISION.md.gz"):
        try:
            path = doc_root / name
            content = (gzip.open(path, "rt", encoding="utf-8") if name.endswith(".gz")
                       else path.open(encoding="utf-8"))
            with content as stream:
                text = stream.read(16384)
            found = re.search(r"(?:Public )?Git revision:\s*`?([0-9a-f]{40})\b", text)
            if found:
                revision = found.group(1)
                break
        except (OSError, UnicodeError, EOFError):
            pass
    return {"packages": packages, "public_source_revision": revision}


def collect(sys_root=Path("/sys"), proc_root=Path("/proc"),
            etc_root=Path("/etc"), software=None):
    report = {
        "schema_version": 2,
        "collected_at_utc": datetime.now(timezone.utc).replace(microsecond=0).isoformat(),
        "privacy": "Host/user identity fields, home paths, PCI addresses, serials, media, environment dumps and network data are excluded. Review all fields before sharing, especially custom kernel/module values.",
        "system": system_info(etc_root),
        "software": software_info() if software is None else software,
        "issue_url": "https://github.com/royakem/OpenLiveGamer4K/issues/new?template=capture-problem.yml",
        "capture_test_performed": False,
        "reproduction_details_needed": ["HDMI source device and audio format", "input resolution and refresh rate", "capture output format/size/rate", "OBS or application version", "steps, expected result and actual result", "whether restarting audio or disabling it changes the problem"],
        "kernel_version": read_text(proc_root / "sys/kernel/osrelease"),
        "devices": [],
    }
    pci_root = sys_root / "bus/pci/devices"
    try:
        devices = sorted(pci_root.iterdir(), key=lambda item: item.name)
    except OSError:
        devices = []

    for device in devices:
        if read_text(device / "vendor") != PCI_VENDOR or read_text(device / "device") != PCI_DEVICE:
            continue
        addresses = {device.name}
        driver_path = device / "driver"
        try:
            driver = driver_path.resolve(strict=True).name
        except OSError:
            driver = None
        module_dir = None
        try:
            module_dir = (driver_path / "module").resolve(strict=True)
        except OSError:
            pass
        if module_dir is None and driver:
            candidate = sys_root / "module" / driver
            if candidate.exists():
                module_dir = candidate
        params = {}
        if module_dir is not None:
            param_dir = module_dir / "parameters"
            try:
                for parameter in sorted(param_dir.iterdir(), key=lambda item: item.name):
                    value = read_text(parameter)
                    if (parameter.name in PARAMETERS and value is not None
                            and re.fullmatch(r"[YNyn01]|[0-9]+", value)):
                        params[parameter.name] = value
            except OSError:
                pass
        module = {
            "name": driver,
            "srcversion": read_text(module_dir / "srcversion") if module_dir else None,
            "parameters": params,
        }
        report["devices"].append({
            "vendor": PCI_VENDOR,
            "device": PCI_DEVICE,
            "bound_module": module,
            "audio_status": parse_status(read_text(device / "audio_status")),
            "matching_alsa_cards": alsa_cards(sys_root, proc_root, addresses),
        })
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", help="user-chosen JSON report file; existing files are not overwritten")
    parser.add_argument("--sysfs-root", type=Path, default=Path("/sys"), help=argparse.SUPPRESS)
    parser.add_argument("--proc-root", type=Path, default=Path("/proc"), help=argparse.SUPPRESS)
    args = parser.parse_args()
    output = Path(args.output).expanduser()
    try:
        # Exclusive creation avoids silently replacing a report the user chose to keep.
        fd = os.open(output, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(fd, "w", encoding="utf-8") as stream:
            json.dump(collect(args.sysfs_root, args.proc_root), stream, indent=2, sort_keys=True)
            stream.write("\n")
    except FileExistsError:
        parser.exit(1, "output file already exists; choose a different filename\n")
    except OSError as exc:
        parser.exit(1, f"cannot write report ({exc.strerror})\n")
    print(f"Wrote diagnostics report to {output}")


if __name__ == "__main__":
    main()
