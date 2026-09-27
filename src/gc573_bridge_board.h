/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef GC573_BRIDGE_BOARD_H
#define GC573_BRIDGE_BOARD_H
struct gc573_device;
int gc573_bridge_board_prepare(struct gc573_device *dev);
int gc573_bridge_program_edid(struct gc573_device *dev);
#endif
