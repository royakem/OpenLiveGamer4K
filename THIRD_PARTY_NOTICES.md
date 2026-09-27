# Third-party source and research references

OpenLiveGamer4K is distributed under GPL-2.0-only; see LICENSE. Kernel source
files carry SPDX identifiers. This implementation is informed by published
GPL source, hardware observations and reverse engineering; it is not described
as a clean-room implementation.

## GC555 Linux driver

Adapted register sequences and algorithms originate in the GPL-2.0-only
[ostrich/gc555 project](https://github.com/ostrich/gc555), reference revision
`7bb51508742adf750b17756b2211ca2bb6eeb361`.

- IT6664 clock, RX acquisition, EQ, TX, routing, EDID and SCDC logic: adapted
  from `gc555-it6664-core.c` and `gc555-it6664-tx.c` for the GC573's topology.
- IT6805 receiver calibration/output behavior, audio status decoding, N/CTS/TMDS
  audio clock measurement/setup and audio output tristate control: adapted from
  `gc555-it6805-core.c`, with board-specific sequences and measured profiles.
- FPGA color matrices and scaler coefficients: correspond to
  `gc555-fpga.c`; register programming and scaling phases are independently
  integrated with the GC573 PCI/DMA path.

The reference source identifies its license through GPL-2.0-only SPDX markers;
its repository's COPYING supplies the GPL version 2 text. Upstream authorship
and attribution remain with the GC555 contributors. Modifications for GC573
are in this project, dated 2026.

## Linux interfaces and timing definitions

The module uses Linux kernel APIs and the kernel's V4L2 timing definitions.
Kernel headers are build dependencies and are not vendored here.

## Hardware investigation

GC573 vendor objects were inspected and exercised in an offline research
harness to observe register behavior, format selectors and scaler writes.
Those objects, their compiled code, and the vendor driver are not included
in this distribution or needed to build/load this module. A measured EDID
is embedded as hardware-description data in `src/gc573_edid.c`.

AVerMedia and Live Gamer are names of their respective owner. This is an
independent driver project and does not claim vendor endorsement.
