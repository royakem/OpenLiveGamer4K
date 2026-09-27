/* SPDX-License-Identifier: GPL-2.0-only */
/* Direct /dev/videoN backend. Production path. */
#include "olg4k/backend.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/poll.h>
#include <unistd.h>

static int real_open(const char *path, void *impl, Olg4kErr *err, int *out_fd)
{
	(void)impl;
	int fd = open(path, O_RDWR | O_CLOEXEC | O_NONBLOCK);
	if (fd < 0) {
		*out_fd = errno;
		switch (errno) {
			case EACCES:
			case EPERM: *err = OLG4K_ERR_PERMISSION; break;
			case ENOENT:
			case ENODEV: *err = OLG4K_ERR_NOT_FOUND; break;
			default: *err = OLG4K_ERR_OPEN; break;
		}
		return -1;
	}
	*out_fd = fd;
	return 0;
}

static long real_ioctl(int fd, unsigned long request, void *arg, void *impl,
	int *err_out)
{
	(void)impl;
	if (ioctl(fd, request, arg) < 0) {
		*err_out = errno;
		return -1;
	}
	return 0;
}

static int real_close(int fd, void *impl)
{
	(void)impl;
	return close(fd);
}

static int real_mmap(int fd, void **addr, size_t length, int prot, int flags,
	off_t offset, void *impl, Olg4kErr *err, int *err_out)
{
	(void)impl;
	void *p = mmap(NULL, length, prot, flags, fd, offset);
	if (p == MAP_FAILED) {
		*err_out = errno;
		*err = OLG4K_ERR_BACKEND;
		return -1;
	}
	*addr = p;
	return 0;
}

static int real_munmap(void *addr, size_t length, void *impl)
{
	(void)impl;
	return munmap(addr, length);
}

static int real_poll(int fd, void *impl, struct pollfd *fds, nfds_t nfds,
	int timeout_ms)
{
	(void)fd;
	(void)impl;
	return poll(fds, nfds, timeout_ms);
}

const Olg4kBackend olg4k_backend_real = {
	.open = real_open,
	.ioctl = real_ioctl,
	.close = real_close,
	.mmap = real_mmap,
	.munmap = real_munmap,
	.poll_fd = real_poll,
	.name = "direct-v4l2",
};
