/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef GC573_BRIDGE_TX_H
#define GC573_BRIDGE_TX_H

struct gc573_device;

/* Prepare one IT6664 TX port (0..3) without selecting a switch route. */
int gc573_bridge_tx_prepare(struct gc573_device *dev, unsigned int port);

/* Enable the checked TX1 (board address 0x6a) native RGB8 path.
 * Requires parent switch/common initialization, measured upstream EDID and
 * RX acquisition and confirmed TX1 DDC/link presence. TX1 HPD is managed by
 * the board/IT6805 parent path; no GC573 GPIO HPD control is assumed here.
 */
int gc573_bridge_tx_enable_native(struct gc573_device *dev);

/* Recheck current checked AVI/raster/depth before starting native capture. */
int gc573_bridge_validate_native(struct gc573_device *dev);

/* Fast AVI/raster change detector for the parent's capture recovery path. */
int gc573_bridge_check_mode(struct gc573_device *dev);

#endif /* GC573_BRIDGE_TX_H */
