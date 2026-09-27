// SPDX-License-Identifier: GPL-2.0-only
#include <linux/errno.h>
#include <linux/i2c.h>
#include <linux/io.h>
#include <linux/jiffies.h>
#include <linux/delay.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/string.h>

#include "gc573_pure.h"

/*
 * The FPGA contains a byte command engine at 0x10c/0x12c and a higher level
 * board transaction engine at 0x184..0x1a4. The functioning GC573 driver uses
 * the latter for the receiver callbacks. Keeping this path separate from the
 * Linux adapter wrapper makes the address convention explicit: board callers
 * pass the even 8-bit address byte (the IT6805 is 0x90), while Linux callers
 * use the normal 7-bit address and are converted at the adapter boundary.
 */
#define GC573_I2C_BUS0_CLOCK		0x120
#define GC573_I2C_BUS0_CLOCK_HI		0x124
#define GC573_I2C_BUS0_CLOCK_CTRL	0x128
#define GC573_I2C_BUS0_ROUTE		0x180

#define GC573_I2C_SLAVE			0x184
#define GC573_I2C_SUB_ADDRESS_HIGH	0x188
#define GC573_I2C_SUB_ADDRESS_LOW	0x18c
#define GC573_I2C_SUB_ADDRESS		0x190
#define GC573_I2C_DATA_COUNT		0x194
#define GC573_I2C_DATA			0x198
#define GC573_I2C_READ_DATA		0x19c
#define GC573_I2C_COMMAND		0x1a0
#define GC573_I2C_COMPLETION		0x1a4

#define GC573_I2C_IRQ_BIT		BIT(11)
#define GC573_I2C_SETUP			0x10
#define GC573_I2C_READ_START		0x08
#define GC573_I2C_WRITE_START		0x04
#define GC573_I2C_POLL_COUNT		1000

static int gc573_i2c_validate_address(u16 address)
{
	if ((address & 1) || address > 0xfe)
		return -EINVAL;
	return 0;
}

static void gc573_i2c_arm_completion(struct gc573_device *dev)
{
	unsigned long flags;

	spin_lock_irqsave(&dev->i2c_state_lock, flags);
	dev->i2c_completion_status = 0;
	dev->i2c_waiting = dev->hw.irq_requested;
	reinit_completion(&dev->i2c_completion);
	spin_unlock_irqrestore(&dev->i2c_state_lock, flags);
}

/* Called from the hard IRQ path before status bit 11 is acknowledged. */
void gc573_i2c_irq(struct gc573_device *dev)
{
	unsigned long flags;
	u8 completion;

	if (!dev)
		return;

	spin_lock_irqsave(&dev->i2c_state_lock, flags);
	if (!dev->i2c_waiting) {
		spin_unlock_irqrestore(&dev->i2c_state_lock, flags);
		return;
	}
	completion = readb(dev->mmio.base + GC573_I2C_COMPLETION);
	dev->i2c_completion_status = completion;
	dev->i2c_waiting = false;
	complete(&dev->i2c_completion);
	spin_unlock_irqrestore(&dev->i2c_state_lock, flags);
}

static int gc573_i2c_wait_completion(struct gc573_device *dev, u8 *status)
{
	unsigned long flags;
	u32 irq_status;
	unsigned int tries;
	long waited;

	if (dev->hw.irq_requested) {
		waited = wait_for_completion_timeout(&dev->i2c_completion,
						msecs_to_jiffies(1000));
		spin_lock_irqsave(&dev->i2c_state_lock, flags);
		*status = dev->i2c_completion_status;
		dev->i2c_waiting = false;
		spin_unlock_irqrestore(&dev->i2c_state_lock, flags);
		return waited ? 0 : -ETIMEDOUT;
	}

	/* Probe runs before request_irq. Poll the same status source that the
	 * reference interrupt handler uses, then acknowledge it literally. */
	for (tries = 0; tries < GC573_I2C_POLL_COUNT; tries++) {
		if (gc573_mmio_read32(&dev->mmio, GC573_REG_IRQ_STATUS,
					      &irq_status))
			return -EIO;
		if (irq_status & GC573_I2C_IRQ_BIT) {
			*status = readb(dev->mmio.base + GC573_I2C_COMPLETION);
			dev_dbg(&dev->pdev->dev,
				 "I2C polled completion irq=0x%08x byte=0x%02x\n",
				 irq_status, *status);
			return gc573_mmio_write32(&dev->mmio, GC573_REG_IRQ_STATUS,
						  GC573_I2C_IRQ_BIT);
		}
		usleep_range(500, 1000);
	}

	return -ETIMEDOUT;
}

static int gc573_i2c_finish(struct gc573_device *dev)
{
	u8 status;
	int ret;

	ret = gc573_i2c_wait_completion(dev, &status);
	if (ret)
		return ret;
	/* The reference treats completion bits 0 and 2 as successful status. */
	dev_dbg(&dev->pdev->dev, "I2C completion byte=0x%02x\n", status);
	if (!(status & (BIT(0) | BIT(2))))
		return -EIO;
	return 0;
}

static int gc573_i2c_controller_read_locked(struct gc573_device *dev,
						 u16 address, u8 reg,
						 u8 *buffer, u8 length)
{
	unsigned int i;
	int ret;

	ret = gc573_i2c_validate_address(address);
	if (ret || !buffer || !length)
		return ret ? ret : -EINVAL;

	/* This ordering mirrors aver_xilinx_I2CRead. */
	ret = gc573_mmio_write32(&dev->mmio, GC573_I2C_SLAVE,
				 address | 1);
	if (ret)
		return ret;
	ret = gc573_mmio_write32(&dev->mmio, GC573_I2C_SUB_ADDRESS_HIGH, 0);
	if (ret)
		return ret;
	ret = gc573_mmio_write32(&dev->mmio, GC573_I2C_SUB_ADDRESS, reg);
	if (ret)
		return ret;
	ret = gc573_mmio_write32(&dev->mmio, GC573_I2C_SUB_ADDRESS_LOW, 0);
	if (ret)
		return ret;
	ret = gc573_mmio_write32(&dev->mmio, GC573_I2C_DATA_COUNT, length);
	if (ret)
		return ret;
	ret = gc573_mmio_write32(&dev->mmio, GC573_I2C_COMMAND,
				 GC573_I2C_SETUP);
	if (ret)
		return ret;

	gc573_i2c_arm_completion(dev);
	ret = gc573_mmio_write32(&dev->mmio, GC573_I2C_COMPLETION, 0);
	if (ret)
		return ret;
	ret = gc573_mmio_write32(&dev->mmio, GC573_I2C_COMMAND,
				 GC573_I2C_READ_START);
	if (ret)
		return ret;
	ret = gc573_i2c_finish(dev);
	if (ret)
		return ret;

	/* The reference returns to setup mode before consuming the data FIFO. */
	ret = gc573_mmio_write32(&dev->mmio, GC573_I2C_COMMAND,
				 GC573_I2C_SETUP);
	if (ret)
		return ret;
	for (i = 0; i < length; i++) {
		u32 value;

		/* aver_xilinx_I2CRead consumes this FIFO with ReadReg (32-bit),
		 * not ReadRegByte. The distinction is observable on this FPGA. */
		ret = gc573_mmio_read32(&dev->mmio, GC573_I2C_READ_DATA, &value);
		if (ret)
			return ret;
		buffer[i] = (u8)value;
	}
	return 0;
}

static int gc573_i2c_controller_write_locked(struct gc573_device *dev,
						  u16 address, u8 reg,
						  const u8 *buffer, u8 length)
{
	unsigned int i;
	int ret;

	ret = gc573_i2c_validate_address(address);
	if (ret || (length && !buffer))
		return ret ? ret : -EINVAL;

	/* This ordering mirrors aver_xilinx_I2CWrite. */
	ret = gc573_mmio_write32(&dev->mmio, GC573_I2C_SLAVE,
				 address & 0xfe);
	if (ret)
		return ret;
	ret = gc573_mmio_write32(&dev->mmio, GC573_I2C_SUB_ADDRESS_HIGH, 0);
	if (ret)
		return ret;
	ret = gc573_mmio_write32(&dev->mmio, GC573_I2C_SUB_ADDRESS, reg);
	if (ret)
		return ret;
	ret = gc573_mmio_write32(&dev->mmio, GC573_I2C_COMMAND,
				 GC573_I2C_SETUP);
	if (ret)
		return ret;
	ret = gc573_mmio_write32(&dev->mmio, GC573_I2C_SUB_ADDRESS_LOW, 0);
	if (ret)
		return ret;
	ret = gc573_mmio_write32(&dev->mmio, GC573_I2C_DATA_COUNT, length);
	if (ret)
		return ret;
	for (i = 0; i < length; i++) {
		ret = gc573_mmio_write32(&dev->mmio, GC573_I2C_DATA,
					 buffer[i]);
		if (ret)
			return ret;
	}

	gc573_i2c_arm_completion(dev);
	ret = gc573_mmio_write32(&dev->mmio, GC573_I2C_COMPLETION, 0);
	if (ret)
		return ret;
	ret = gc573_mmio_write32(&dev->mmio, GC573_I2C_COMMAND,
				 GC573_I2C_WRITE_START);
	if (ret)
		return ret;
	return gc573_i2c_finish(dev);
}

static int gc573_i2c_master_xfer(struct i2c_adapter *adapter,
				 struct i2c_msg *msgs, int num)
{
	struct gc573_i2c_bus *bus = i2c_get_adapdata(adapter);
	struct gc573_device *dev;
	u8 pending_reg = 0;
	u16 pending_address = 0;
	bool have_pending_reg = false;
	int i;
	int ret = 0;

	if (!bus || !msgs || num <= 0)
		return -EINVAL;
	dev = container_of(bus, struct gc573_device, i2c_bus);

	mutex_lock(&bus->lock);
	for (i = 0; i < num; i++) {
		struct i2c_msg *msg = &msgs[i];
		u16 address;

		if (msg->flags & (I2C_M_TEN | I2C_M_RECV_LEN) ||
			msg->addr > 0x7f || msg->len > U8_MAX) {
			ret = -EOPNOTSUPP;
			break;
		}
		address = (u16)msg->addr << 1;

		if (msg->flags & I2C_M_RD) {
			pending_reg = (have_pending_reg &&
					pending_address == address) ? pending_reg : 0;
			ret = gc573_i2c_controller_read_locked(dev, address,
								pending_reg,
								msg->buf, msg->len);
			have_pending_reg = false;
		} else if (msg->len) {
			pending_reg = msg->buf[0];
			pending_address = address;
			have_pending_reg = true;
			/* A one-byte write followed by a read is the standard register
			 * read form. Defer it so the controller can issue its own read
			 * transaction with the same sub-address. */
			if (i + 1 == num ||
				!(msgs[i + 1].flags & I2C_M_RD)) {
				ret = gc573_i2c_controller_write_locked(dev, address,
								pending_reg,
								msg->len > 1 ? &msg->buf[1] : NULL,
								(u8)(msg->len - 1));
				have_pending_reg = false;
			}
		} else {
			ret = gc573_i2c_controller_write_locked(dev, address, 0,
								 NULL, 0);
			have_pending_reg = false;
		}
		if (ret)
			break;
	}
	mutex_unlock(&bus->lock);

	return ret ? ret : num;
}

static u32 gc573_i2c_functionality(struct i2c_adapter *adapter)
{
	return I2C_FUNC_I2C;
}

static const struct i2c_algorithm gc573_i2c_algorithm = {
	.master_xfer = gc573_i2c_master_xfer,
	.functionality = gc573_i2c_functionality,
};

/* Bus-0 setup observed in aver_xilinx_set_i2c_speed. */
static int gc573_i2c_configure_bus0(struct gc573_device *dev)
{
	int ret;

	ret = gc573_mmio_write32(&dev->mmio, GC573_I2C_BUS0_CLOCK, 0x3d);
	if (ret)
		return ret;
	ret = gc573_mmio_write32(&dev->mmio, GC573_I2C_BUS0_CLOCK_HI, 0);
	if (ret)
		return ret;
	ret = gc573_mmio_write32(&dev->mmio, GC573_I2C_BUS0_CLOCK_CTRL, 0x80);
	if (ret)
		return ret;
	return gc573_mmio_write32(&dev->mmio, GC573_I2C_BUS0_ROUTE, 0x138);
}

int gc573_i2c_read_block(struct gc573_device *dev, u16 address, u8 reg,
			 u8 *buffer, u16 length)
{
	int ret;

	if (!dev || !dev->i2c_bus.registered || !buffer || !length ||
		length > U8_MAX)
		return -EINVAL;
	ret = gc573_i2c_validate_address(address);
	if (ret)
		return ret;

	mutex_lock(&dev->i2c_bus.lock);
	ret = gc573_i2c_controller_read_locked(dev, address, reg, buffer,
						       (u8)length);
	mutex_unlock(&dev->i2c_bus.lock);
	return ret;
}

int gc573_i2c_read_reg(struct gc573_device *dev, u16 address, u8 reg,
			       u8 *value)
{
	return gc573_i2c_read_block(dev, address, reg, value, 1);
}

int gc573_i2c_write_reg(struct gc573_device *dev, u16 address, u8 reg,
				 u8 value)
{
	int ret;

	if (!dev || !dev->i2c_bus.registered)
		return -EINVAL;
	ret = gc573_i2c_validate_address(address);
	if (ret)
		return ret;

	mutex_lock(&dev->i2c_bus.lock);
	ret = gc573_i2c_controller_write_locked(dev, address, reg, &value, 1);
	mutex_unlock(&dev->i2c_bus.lock);
	return ret;
}

int gc573_i2c_register(struct gc573_device *dev)
{
	struct gc573_i2c_bus *bus;
	int ret;

	if (!dev || !dev->mmio.base)
		return -EINVAL;

	bus = &dev->i2c_bus;
	memset(bus, 0, sizeof(*bus));
	bus->channel = 0;
	mutex_init(&bus->lock);
	init_completion(&dev->i2c_completion);
	spin_lock_init(&dev->i2c_state_lock);
	dev->i2c_waiting = false;
	dev->i2c_completion_status = 0;

	ret = gc573_i2c_configure_bus0(dev);
	if (ret)
		return ret;
	bus->adapter.owner = THIS_MODULE;
	bus->adapter.algo = &gc573_i2c_algorithm;
	bus->adapter.dev.parent = &dev->pdev->dev;
	strscpy(bus->adapter.name, "GC573 FPGA I2C bus 0",
			sizeof(bus->adapter.name));
	i2c_set_adapdata(&bus->adapter, bus);

	ret = i2c_add_adapter(&bus->adapter);
	if (ret)
		return ret;

	bus->registered = true;
	dev_info(&dev->pdev->dev,
		 "registered FPGA board-transaction I2C bus 0\n");
	return 0;
}

void gc573_i2c_unregister(struct gc573_device *dev)
{
	unsigned long flags;

	if (!dev || !dev->i2c_bus.registered)
		return;

	spin_lock_irqsave(&dev->i2c_state_lock, flags);
	dev->i2c_waiting = false;
	spin_unlock_irqrestore(&dev->i2c_state_lock, flags);
	i2c_del_adapter(&dev->i2c_bus.adapter);
	dev->i2c_bus.registered = false;
}
