// SPDX-License-Identifier: MIT
/*
 * memdump -- read-only dump of whitelisted i.MX8M physical regions,
 * used for imx-sectool Phase 0 discovery (see DESIGN.md, section 10).
 *
 * Only the regions in the table below can be read: the tool never takes a
 * user-supplied address, never writes, and maps one region at a time through
 * /dev/mem with PROT_READ. Reads are aligned 32-bit volatile loads, because
 * /dev/mem with O_SYNC gives a Device mapping on arm64 and wider or unaligned
 * accesses (as memcpy may issue) are not safe there.
 *
 *   rom-low    0x00000000  0x1000   Boot ROM: HAB RVT (0x900), ROM API (0x980),
 *                                   ROM log pointer (0x9e0)
 *   ocram-low  0x00900000  0x40000  OCRAM below TF-A (NXP BL31 at 0x950000);
 *                                   ROM log buffer and ROM/HAB persistent data
 *                          0x20000  on i.MX8MM (BL31 at 0x920000)
 *   src        0x30390000  0x100    SRC: SBMR1, SRSR, SBMR2, GPR1..GPR10
 *   snvs-hp    0x30370000  0x100    SNVS HP: HPSR (SSM state) and friends
 *
 * Safety: a region whose clock is gated, or which is TrustZone-protected, can
 * hang the bus or raise an SError. The regions above were chosen to avoid
 * TF-A's OCRAM area; the phase0.sh wrapper checks clocks before calling us.
 *
 * Deps: libc only.
 *
 * Author: Stefano Viola <stefano.viola@toradex.com>
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define VERSION "0.1.0"

/* exit codes */
#define EX_OK 0
#define EX_USAGE 2
#define EX_HW 3
#define EX_STATE 4

struct region {
	const char *name;
	uint32_t base;
	uint32_t size;
	const char *desc;
};

/* addresses from U-Boot/TF-A, see DESIGN.md section 8 */
static const struct region imx8mp_regions[] = {
	{ "rom-low", 0x00000000, 0x1000,
	  "Boot ROM low page (RVT, ROM API, ROM log ptr)" },
	{ "ocram-low", 0x00900000, 0x40000, "OCRAM below TF-A" },
	{ "src", 0x30390000, 0x100, "System Reset Controller" },
	{ "snvs-hp", 0x30370000, 0x100, "SNVS high-power domain" },
};

static const struct region imx8mm_regions[] = {
	{ "rom-low", 0x00000000, 0x1000,
	  "Boot ROM low page (RVT, ROM API, ROM log ptr)" },
	{ "ocram-low", 0x00900000, 0x20000, "OCRAM below TF-A" },
	{ "src", 0x30390000, 0x100, "System Reset Controller" },
	{ "snvs-hp", 0x30370000, 0x100, "SNVS high-power domain" },
};

static const struct region *regions;
static size_t n_regions;

#define SOC_ID_PATH "/sys/devices/soc0/soc_id"

static void usage(FILE *f)
{
	fprintf(f,
		"usage: memdump list\n"
		"       memdump dump <region> <outfile>     raw little-endian binary\n"
		"       memdump hex  <region> [<off> <len>] hexdump to stdout\n"
		"       memdump --version\n");
}

static const struct region *find_region(const char *name)
{
	size_t i;

	for (i = 0; i < n_regions; i++)
		if (!strcmp(regions[i].name, name))
			return &regions[i];
	return NULL;
}

/* pick the region table for this SoC, refuse any other SoC */
static int check_soc(void)
{
	char buf[64] = { 0 };
	FILE *f;

	f = fopen(SOC_ID_PATH, "r");
	if (!f) {
		fprintf(stderr, "error: cannot read %s (%s)\n", SOC_ID_PATH,
			strerror(errno));
		return EX_STATE;
	}
	if (!fgets(buf, sizeof(buf), f))
		buf[0] = '\0';
	fclose(f);
	buf[strcspn(buf, "\n")] = '\0';
	if (!strcmp(buf, "i.MX8MP")) {
		regions = imx8mp_regions;
		n_regions = ARRAY_SIZE(imx8mp_regions);
	} else if (!strcmp(buf, "i.MX8MM")) {
		regions = imx8mm_regions;
		n_regions = ARRAY_SIZE(imx8mm_regions);
	} else {
		fprintf(stderr, "error: SoC '%s' is not supported\n", buf);
		return EX_STATE;
	}
	return EX_OK;
}

/* ---- /dev/mem mapping ------------------------------------------------- */

/*
 * Map a region read-only. Returns the mapping or NULL; *fd_out must be closed
 * by the caller after munmap().
 */
static volatile uint32_t *map_region(const struct region *r, int *fd_out)
{
	void *p;
	int fd;

	fd = open("/dev/mem", O_RDONLY | O_SYNC);
	if (fd < 0) {
		fprintf(stderr,
			"error: cannot open /dev/mem (%s)\n"
			"hint:  run as root; the kernel needs CONFIG_DEVMEM\n",
			strerror(errno));
		return NULL;
	}
	p = mmap(NULL, r->size, PROT_READ, MAP_SHARED, fd, (off_t)r->base);
	if (p == MAP_FAILED) {
		fprintf(stderr,
			"error: mmap of %s @0x%08x failed (%s)\n"
			"hint:  STRICT_DEVMEM / IO_STRICT_DEVMEM may block it; check /proc/iomem for a driver claiming the range\n",
			r->name, r->base, strerror(errno));
		close(fd);
		return NULL;
	}
	*fd_out = fd;
	return (volatile uint32_t *)p;
}

static void unmap_region(const struct region *r, volatile uint32_t *p, int fd)
{
	munmap((void *)p, r->size);
	close(fd);
}

/* ---- commands ----------------------------------------------------------- */

static int cmd_list(void)
{
	size_t i;

	for (i = 0; i < n_regions; i++)
		printf("%-10s 0x%08x 0x%06x  %s\n", regions[i].name,
		       regions[i].base, regions[i].size, regions[i].desc);
	return EX_OK;
}

static int cmd_dump(const struct region *r, const char *out)
{
	volatile uint32_t *p;
	int rc = EX_OK;
	uint32_t i, v;
	FILE *f;
	int fd;

	p = map_region(r, &fd);
	if (!p)
		return EX_HW;
	f = fopen(out, "wb");
	if (!f) {
		fprintf(stderr, "error: cannot create %s (%s)\n", out,
			strerror(errno));
		unmap_region(r, p, fd);
		return EX_STATE;
	}
	for (i = 0; i < r->size / 4; i++) {
		v = p[i]; /* one aligned 32-bit load per word */
		if (fwrite(&v, sizeof(v), 1, f) != 1) {
			rc = EX_STATE;
			break;
		}
	}
	if (fclose(f) != 0)
		rc = EX_STATE;
	unmap_region(r, p, fd);
	if (rc != EX_OK)
		fprintf(stderr, "error: write to %s failed\n", out);
	return rc;
}

static int cmd_hex(const struct region *r, uint32_t off, uint32_t len)
{
	volatile uint32_t *p;
	uint32_t o;
	int fd;

	if ((off & 3) || (len & 3) || off >= r->size || len > r->size - off) {
		fprintf(stderr,
			"error: range 0x%x+0x%x outside %s or not word aligned\n",
			off, len, r->name);
		return EX_USAGE;
	}
	p = map_region(r, &fd);
	if (!p)
		return EX_HW;
	for (o = off; o < off + len; o += 4) {
		if (((o - off) % 16) == 0)
			printf("%s%08x:", o == off ? "" : "\n", r->base + o);
		printf(" %08x", p[o / 4]);
	}
	printf("\n");
	unmap_region(r, p, fd);
	return EX_OK;
}

static int parse_u32(const char *s, uint32_t *v)
{
	unsigned long x;
	char *ep;

	errno = 0;
	x = strtoul(s, &ep, 0);
	if (errno || *ep || x > UINT32_MAX)
		return -1;
	*v = (uint32_t)x;
	return 0;
}

int main(int argc, char **argv)
{
	const struct region *r;
	uint32_t off, len;
	const char *cmd;
	int rc;

	if (argc < 2) {
		usage(stderr);
		return EX_USAGE;
	}
	cmd = argv[1];

	if (!strcmp(cmd, "--version")) {
		printf("memdump %s\n", VERSION);
		return EX_OK;
	}
	if (!strcmp(cmd, "-h") || !strcmp(cmd, "--help")) {
		usage(stdout);
		return EX_OK;
	}
	rc = check_soc();
	if (rc != EX_OK)
		return rc;
	if (!strcmp(cmd, "list"))
		return cmd_list();

	if (argc < 3) {
		usage(stderr);
		return EX_USAGE;
	}
	r = find_region(argv[2]);
	if (!r) {
		fprintf(stderr,
			"error: unknown region '%s' (see 'memdump list')\n",
			argv[2]);
		return EX_USAGE;
	}

	if (!strcmp(cmd, "dump")) {
		if (argc != 4) {
			usage(stderr);
			return EX_USAGE;
		}
		return cmd_dump(r, argv[3]);
	}
	if (!strcmp(cmd, "hex")) {
		off = 0;
		len = r->size;
		if (argc == 5) {
			if (parse_u32(argv[3], &off) ||
			    parse_u32(argv[4], &len)) {
				usage(stderr);
				return EX_USAGE;
			}
		} else if (argc != 3) {
			usage(stderr);
			return EX_USAGE;
		}
		return cmd_hex(r, off, len);
	}
	usage(stderr);
	return EX_USAGE;
}
