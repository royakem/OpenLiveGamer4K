/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef GC573_ACQUIRE_H
#define GC573_ACQUIRE_H

struct gc573_device;

/* Service one bounded IT6805 port-0 SYS/EQ acquisition pass. */
int gc573_it6805_service_acquisition(struct gc573_device *dev);

/* Run the shipped port-0 Trigger_EQ register sequence once. */
int gc573_it6805_trigger_eq(struct gc573_device *dev);

#endif /* GC573_ACQUIRE_H */
