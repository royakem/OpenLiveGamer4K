#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Write a passive, redacted GC573 HDMI-audio diagnostics report as JSON."""

import argparse
import json
from pathlib import Path
import re
from datetime import datetime, timezone


PCI_VENDOR = "0x1461"
PCI_DEVICE = "0x0054"
PARAMETERS = {"audio_experimental", "audio_dma_probe", "bridge_link", "bridge_bootstrap",
              "bridge_tx_only", "preserve_bridge", "preserve_receiver", "reset_gpio_mask",
              "test_hpd_after_ms", "trace_frames"}


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
            values[match.group(1)] = match.group(2)
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
    proc_cards = read_text(proc_root / "asound/cards") or ""
    names = {}
    for line in proc_cards.splitlines():
        match = re.match(r"\s*(\d+)\s+\[([^]]+)\]:\s*(.*)", line)
        if match:
            names[match.group(1)] = {"id": match.group(2).strip(), "name": match.group(3).strip()}
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
        item.update(names.get(number, {}))
        card_id = read_text(entry / "id")
        if card_id and "id" not in item:
            item["id"] = card_id
        cards.append(item)
    return cards


def collect(sys_root=Path("/sys"), proc_root=Path("/proc")):
    report = {
        "schema_version": 1,
        "collected_at_utc": datetime.now(timezone.utc).replace(microsecond=0).isoformat(),
        "privacy": "No hostname, username, home path, PCI address, serial, audio samples, or network data collected.",
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
        with output.open("x", encoding="utf-8") as stream:
            json.dump(collect(args.sysfs_root, args.proc_root), stream, indent=2, sort_keys=True)
            stream.write("\n")
    except FileExistsError:
        parser.exit(1, "output file already exists; choose a different filename\n")
    except OSError as exc:
        parser.exit(1, f"cannot write report ({exc.strerror})\n")
    print(f"Wrote diagnostics report to {output}")


if __name__ == "__main__":
    main()
