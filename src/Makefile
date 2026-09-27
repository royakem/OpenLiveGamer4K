obj-m += gc573_pure.o

gc573_pure-y := gc573_pci.o gc573_mmio.o gc573_dma.o gc573_desc.o \
	gc573_hw.o gc573_fpga.o gc573_scaler.o gc573_i2c.o gc573_v4l2.o gc573_receiver.o gc573_acquire.o \
	gc573_output.o gc573_edid.o gc573_bridge_clock.o gc573_bridge_rx.o gc573_bridge_tx.o gc573_bridge_board.o gc573_bridge_acquire.o gc573_bridge_eq.o gc573_bridge_recover.o

ccflags-y += -Werror=implicit-function-declaration
gc573_pure-y += gc573_audio.o gc573_audio_dma.o

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

.PHONY: all clean
all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
