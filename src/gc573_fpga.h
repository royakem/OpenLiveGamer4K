/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef GC573_FPGA_H
#define GC573_FPGA_H

struct gc573_device;

int gc573_fpga_init_registers(struct gc573_device *dev);
int gc573_fpga_configure_capture(struct gc573_device *dev,
				 u32 input_width, u32 input_height,
				 u32 width, u32 height, u32 rate_num, u32 rate_den,
					 u32 pixelformat);

#endif /* GC573_FPGA_H */
