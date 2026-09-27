/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef GC573_BRIDGE_EQ_H
#define GC573_BRIDGE_EQ_H

#include <linux/types.h>

struct gc573_device;

int gc573_bridge_eq20_run(struct gc573_device *dev, u8 status14);

#endif /* GC573_BRIDGE_EQ_H */
