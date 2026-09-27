/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef GC573_EDID_H
#define GC573_EDID_H

#include <linux/types.h>

struct gc573_device;

/* Captured receiver EDID with computed wire-format checksums. */
int gc573_it6805_get_edid(u8 *edid, size_t size);

/*
 * Program the board-captured 256-byte EDID into IT6805 EDID RAM and update
 * its separate checksum/physical-address registers. Caller owns source HPD:
 * hold GPIO2 low before receiver initialization and raise it after init.
 * This routine never changes HPD.
 */
int gc573_it6805_program_edid(struct gc573_device *dev);

/*
 * Data provenance: byte-for-byte capture from
 * evidence/receiver-edid-20260924.bin (256 raw EDID RAM bytes). Its bytes at
 * offsets 127 and 255 are zero because the receiver handles EDID checksums
 * separately. Program the 127 data bytes per block unchanged; calculated
 * checksums are written to receiver registers, not substituted into this
 * captured payload. Preserve this EDID during bring-up to avoid changing the
 * Windows display mode advertised by the board.
 */

#endif /* GC573_EDID_H */
