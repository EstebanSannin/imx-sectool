// SPDX-License-Identifier: MIT
/*
 * imx-sectool -- read-only physical memory access (classes B and C).
 *
 * Only the regions listed in the SoC descriptor can be read. Each region is
 * mapped read-only through /dev/mem on first use and read with aligned 32-bit
 * loads (O_SYNC gives a Device mapping on arm64, where wider or unaligned
 * accesses are not safe). Nothing is ever written.
 *
 * For host testing, IMX_SECTOOL_MEM_DUMP=<dir> serves reads from
 * <dir>/<region>.bin files instead (the format memdump produces).
 *
 * Author: Stefano Viola <stefano.viola@toradex.com>
 */
#define _POSIX_C_SOURCE 200809L
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>

#include "sectool.h"

#define MAX_REGIONS 8

static volatile uint32_t *maps[MAX_REGIONS];
static char errbuf[256];

const char *mmio_error(void)
{
	return errbuf[0] ? errbuf : "unknown error";
}

static int find_region(uint32_t phys, size_t len)
{
	const struct mem_region *r;
	size_t i;

	if (!soc->n_regions) {
		snprintf(errbuf, sizeof(errbuf),
			 "no memory regions verified on %s yet", soc->name);
		return -1;
	}
	for (i = 0; i < soc->n_regions && i < MAX_REGIONS; i++) {
		r = &soc->regions[i];
		if (phys >= r->base && len <= r->size &&
		    phys - r->base <= r->size - len)
			return (int)i;
	}
	snprintf(errbuf, sizeof(errbuf),
		 "0x%08x+0x%zx is outside the allowed regions", phys, len);
	return -1;
}

/* dump-file backend for host tests */
static int dump_read(const char *dir, const struct mem_region *r, uint32_t off,
		     void *buf, size_t len)
{
	char p[512];
	ssize_t n;
	int fd;

	snprintf(p, sizeof(p), "%s/%s.bin", dir, r->name);
	fd = open(p, O_RDONLY);
	if (fd < 0) {
		snprintf(errbuf, sizeof(errbuf), "cannot open %s (%s)", p,
			 strerror(errno));
		return -1;
	}
	n = pread(fd, buf, len, off);
	close(fd);
	if (n != (ssize_t)len) {
		snprintf(errbuf, sizeof(errbuf), "short read from %s", p);
		return -1;
	}
	return 0;
}

static volatile uint32_t *map_region(int idx)
{
	const struct mem_region *r = &soc->regions[idx];
	void *p;
	int fd, err;

	if (maps[idx])
		return maps[idx];
	fd = open("/dev/mem", O_RDONLY | O_SYNC);
	if (fd < 0) {
		snprintf(errbuf, sizeof(errbuf), "cannot open /dev/mem (%s)%s",
			 strerror(errno),
			 errno == EACCES ? "; run as root" :
			 errno == ENOENT ? "; the kernel needs CONFIG_DEVMEM" :
					   "");
		return NULL;
	}
	p = mmap(NULL, r->size, PROT_READ, MAP_SHARED, fd, (off_t)r->base);
	err = errno;
	close(fd); /* the mapping stays valid */
	if (p == MAP_FAILED) {
		snprintf(errbuf, sizeof(errbuf),
			 "cannot map %s at 0x%08x (%s); STRICT_DEVMEM or a driver claiming the range may block it",
			 r->name, r->base, strerror(err));
		return NULL;
	}
	maps[idx] = (volatile uint32_t *)p;
	return maps[idx];
}

int mmio_read(uint32_t phys, void *buf, size_t len)
{
	const struct mem_region *r;
	volatile uint32_t *m;
	uint32_t *out = buf;
	const char *dir;
	uint32_t off;
	size_t i;
	int idx;

	if ((phys & 3) || (len & 3)) {
		snprintf(errbuf, sizeof(errbuf), "unaligned read 0x%08x+0x%zx",
			 phys, len);
		return -1;
	}
	idx = find_region(phys, len);
	if (idx < 0)
		return -1;
	r = &soc->regions[idx];
	off = phys - r->base;

	dir = getenv("IMX_SECTOOL_MEM_DUMP");
	if (dir)
		return dump_read(dir, r, off, buf, len);

	m = map_region(idx);
	if (!m)
		return -1;
	for (i = 0; i < len / 4; i++)
		out[i] = m[off / 4 + i]; /* one aligned 32-bit load per word */
	return 0;
}

int mmio_read32(uint32_t phys, uint32_t *val)
{
	return mmio_read(phys, val, sizeof(*val));
}
