// SPDX-License-Identifier: MIT
/*
 * imx-sectool -- fuse access through the Linux nvmem framework (class A).
 *
 * The kernel OCOTP driver serializes accesses, enables the OCOTP clock around
 * them and reloads the shadow registers after a write, so this is the only
 * path used for fuses. The nvmem file is word-indexed: offset = index * 4.
 *
 * Author: Stefano Viola <stefano.viola@toradex.com>
 */
#define _POSIX_C_SOURCE 200809L
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>

#include "sectool.h"

#define NVMEM_DIR "/sys/bus/nvmem/devices"

static char path[512];

/* Find the OCOTP nvmem file once: --nvmem, else the first device whose name
 * starts with the SoC's prefix (imx-ocotp0 on i.MX8M).
 */
const char *nvmem_path(void)
{
	struct dirent *e;
	DIR *d;

	if (path[0])
		return path;
	if (opt_nvmem) {
		snprintf(path, sizeof(path), "%s", opt_nvmem);
		return path;
	}
	d = opendir(NVMEM_DIR);
	if (!d)
		return NULL;
	while ((e = readdir(d))) {
		if (!strncmp(e->d_name, soc->nvmem_prefix,
			     strlen(soc->nvmem_prefix))) {
			snprintf(path, sizeof(path), NVMEM_DIR "/%s/nvmem",
				 e->d_name);
			break;
		}
	}
	closedir(d);
	return path[0] ? path : NULL;
}

static int nvmem_open(int flags)
{
	const char *p = nvmem_path();
	int fd;

	if (!p) {
		fprintf(stderr,
			"error: no %s* device in " NVMEM_DIR "\n"
			"hint:  the kernel needs CONFIG_NVMEM_IMX_OCOTP\n",
			soc->nvmem_prefix);
		return -1;
	}
	fd = open(p, flags);
	if (fd < 0)
		fprintf(stderr, "error: cannot open %s (%s)%s\n", p,
			strerror(errno),
			errno == EACCES ? "\nhint:  run as root" : "");
	return fd;
}

int nvmem_read(unsigned int index, uint32_t *val)
{
	int fd = nvmem_open(O_RDONLY);
	ssize_t n;
	int err;

	if (fd < 0)
		return -1;
	n = pread(fd, val, sizeof(*val), (off_t)index * 4);
	err = errno;
	close(fd);
	if (n != (ssize_t)sizeof(*val)) {
		fprintf(stderr, "error: nvmem read of word %u failed (%s)\n",
			index, n < 0 ? strerror(err) : "short read");
		return -1;
	}
	return 0;
}

int nvmem_write(unsigned int index, uint32_t val)
{
	int fd = nvmem_open(O_WRONLY);
	ssize_t n;
	int err;

	if (fd < 0)
		return -1;
	n = pwrite(fd, &val, sizeof(val), (off_t)index * 4);
	err = errno;
	close(fd);
	if (n != (ssize_t)sizeof(val)) {
		fprintf(stderr, "error: nvmem write of word %u failed (%s)\n",
			index, n < 0 ? strerror(err) : "short write");
		return -1;
	}
	return 0;
}
