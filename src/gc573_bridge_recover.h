/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef GC573_BRIDGE_RECOVER_H
#define GC573_BRIDGE_RECOVER_H

struct gc573_device;

/*
 * Check bridge and IT6805 link lock for an active capture. Performs bounded
 * status I2C reads and may write the RX bank selector; it does not reacquire
 * the link or validate the native video mode. Caller serializes process I2C.
 */
int gc573_bridge_check_capture(struct gc573_device *dev);
int gc573_bridge_check_mode(struct gc573_device *dev);

/*
 * Check and, if needed, synchronously reacquire the bridge_link source path
 * before STREAMON. Preserves the receiver output configuration but writes
 * the RX bank selector. Caller must serialize process-context I2C access and
 * have stopped DMA. The parent provides gc573_bridge_validate_native(): a
 * non-mutating validation of the current native RX AVI/raster.
 */
int gc573_bridge_prepare_capture(struct gc573_device *dev);

#endif /* GC573_BRIDGE_RECOVER_H */
