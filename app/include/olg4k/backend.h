/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef OLG4K_BACKEND_H
#define OLG4K_BACKEND_H

#include <poll.h>
#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>
#include <time.h>
#include <linux/types.h>
#include <linux/videodev2.h>

/*
 * V4L2 backend abstraction for the OpenLiveGamer4K control application.
 *
 * Production uses the direct /dev/videoN implementation (v4l2_backend_real);
 * tests link against the deterministic mock (v4l2_backend_mock). Keeping the
 * seam here means mock-only behavior never leaks into the production driver
 * contract.
 *
 * Mode discipline (plan requirement): a status-mode device only issues
 * read-only ioctls. Mutating calls (S_FMT, S_PARM, REQBUFS, QBUF,
 * STREAMON/OFF) are refused by the device wrapper while in status mode so no
 * code path can start a stream from a "read-only" screen.
 */

typedef enum {
	OLG4K_MODE_STATUS = 0, /* read-only queries only */
	OLG4K_MODE_PREVIEW,    /* exclusive capture allowed */
} Olg4kMode;

typedef enum {
	OLG4K_ERR_OK = 0,
	OLG4K_ERR_OPEN,
	OLG4K_ERR_PERMISSION,
	OLG4K_ERR_NOT_FOUND,
	OLG4K_ERR_BUSY,
	OLG4K_ERR_BACKEND,
	OLG4K_ERR_MODE,
	OLG4K_ERR_TIMEOUT,
} Olg4kErr;

typedef struct Olg4kDevice Olg4kDevice;

typedef struct {
	int (*open)(const char *path, void *impl, Olg4kErr *err, int *errno_out);
	long (*ioctl)(int fd, unsigned long request, void *arg, void *impl,
		      int *errno_out);
	int (*close)(int fd, void *impl);
	int (*mmap)(int fd, void **addr, size_t length, int prot, int flags,
		    off_t offset, void *impl, Olg4kErr *err, int *errno_out);
	int (*munmap)(void *addr, size_t length, void *impl);
	int (*poll_fd)(int fd, void *impl, struct pollfd *fds, nfds_t nfds,
		       int timeout_ms);
	const char *name;
} Olg4kBackend;

struct Olg4kDevice {
	const Olg4kBackend *backend;
	void *impl;
	int fd;
	Olg4kMode mode;
	int last_errno;      /* errno of the last failed call, 0 otherwise */
	bool streaming;         /* app-owned stream active (preview mode only) */
	unsigned long long muting_ioctls; /* test counter: mutating calls attempted in status mode */
};

/* Raw ioctl pass-through; sets dev->errno. Returns 0 or -1.
 * Callers must respect dev->mode; the high-level helpers below enforce it. */
int olg4k_raw_ioctl(Olg4kDevice *dev, unsigned long request, void *arg);
int olg4k_open(Olg4kDevice *dev, const Olg4kBackend *backend, void *impl,
		  const char *path, Olg4kMode mode);
void olg4k_close(Olg4kDevice *dev);
void olg4k_set_mode(Olg4kDevice *dev, Olg4kMode mode);
const char *olg4k_err_string(Olg4kErr err);

/* Read-only status helpers. Never mutate capture state. */
int olg4k_querycap(Olg4kDevice *dev, struct v4l2_capability *cap);
int olg4k_enum_fmt(Olg4kDevice *dev, unsigned int index,
		     struct v4l2_fmtdesc *fmt);
int olg4k_enum_framesize(Olg4kDevice *dev, __u32 fourcc, unsigned int index,
			   struct v4l2_frmsizeenum *fsize);
int olg4k_enum_frameinterval(Olg4kDevice *dev, __u32 fourcc,
			      unsigned int width, unsigned int height,
			      unsigned int index,
			      struct v4l2_frmivalenum *fival);
int olg4k_g_fmt(Olg4kDevice *dev, struct v4l2_format *f);
int olg4k_g_parm(Olg4kDevice *dev, struct v4l2_streamparm *parm);
int olg4k_enum_input(Olg4kDevice *dev, unsigned int index,
		      struct v4l2_input *input);

/* Preview helpers. Refused with OLG4K_ERR_MODE unless dev->mode ==
 * OLG4K_MODE_PREVIEW. TRY_FMT is read-only and allowed in both modes. */
int olg4k_try_fmt(Olg4kDevice *dev, struct v4l2_format *f);
int olg4k_s_fmt(Olg4kDevice *dev, struct v4l2_format *f);
int olg4k_s_parm(Olg4kDevice *dev, struct v4l2_streamparm *parm);
int olg4k_reqbufs(Olg4kDevice *dev, struct v4l2_requestbuffers *rb);
int olg4k_qbuf(Olg4kDevice *dev, struct v4l2_buffer *buf);
int olg4k_dqbuf(Olg4kDevice *dev, struct v4l2_buffer *buf);
int olg4k_querybuf(Olg4kDevice *dev, struct v4l2_buffer *buf);
int olg4k_streamon(Olg4kDevice *dev, enum v4l2_buf_type type);
int olg4k_streamoff(Olg4kDevice *dev, enum v4l2_buf_type type);
int olg4k_mmap(Olg4kDevice *dev, void **addr, size_t length, int prot,
	       off_t offset);
int olg4k_munmap(Olg4kDevice *dev, void *addr, size_t length);

#endif /* OLG4K_BACKEND_H */
