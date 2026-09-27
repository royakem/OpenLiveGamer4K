/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef GC573_BRIDGE_ACQUIRE_H
#define GC573_BRIDGE_ACQUIRE_H

struct gc573_device;

/* Prepare upstream HPD; wait for RX SCDT lock and shared clock setup. */
int gc573_bridge_acquire_start(struct gc573_device *dev);
int gc573_bridge_acquire_wait(struct gc573_device *dev);

/* Diagnostic 1500 ms HPD cycle; caller serializes bridge-bank access. */
int gc573_bridge_test_hpd_cycle(struct gc573_device *dev);

#endif /* GC573_BRIDGE_ACQUIRE_H */
