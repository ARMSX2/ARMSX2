// SPDX-License-Identifier: BSD-2-Clause
#include <stdarg.h>
#include "hook_impl.h"

// Loaded global into the driver's namespace: the driver's ioctl calls land
// here and are forwarded exactly as bionic's own ioctl forwards them
// (libc/bionic/ioctl.cpp reads one void* with va_arg unconditionally and
// passes it to __ioctl), so this adds no read bionic does not already make.
// arm64 only (adrenotools refuses other ABIs): the third argument travels in
// x2 whether or not the caller passed one. hook_ioctl touches the argument
// only for IOCTL_KGSL_DRAWCTXT_CREATE on a KGSL device, which always carries
// a pointer.
__attribute__((visibility("default"))) int ioctl(int fd, int request, ...) {
	va_list args;
	va_start(args, request);
	void *arg = va_arg(args, void *);
	va_end(args);
	return hook_ioctl(fd, request, arg);
}
