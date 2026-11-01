// SPDX-License-Identifier: MIT
/*
 * imx-sectool -- optional SCU mailbox access through the stock UIO driver.
 *
 * The mailbox must be dedicated to this tool. Its mapping, driver and
 * power state are checked before access. Only CHIP_INFO and GET_EVENT
 * requests are sent; an incomplete transfer blocks retries until reboot.
 *
 * Author: Stefano Viola <stefano.viola@toradex.com>
 */
#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "ahab.h"

#define MU_SR 0x20
#define MU_CR 0x24
#define MU_MAP_SIZE 4096
#define MU_LOCK "/run/imx-sectool-scu-mu2.lock"

static int fail(struct scu_mu *mu, const char *message)
{
	snprintf(mu->error, sizeof(mu->error), "%s", message);
	return -1;
}

static int read_text(const char *path, char *buf, size_t size)
{
	FILE *file;
	int ok;

	file = fopen(path, "r");
	if (!file)
		return -1;
	ok = fgets(buf, size, file) != NULL;
	fclose(file);
	if (!ok)
		return -1;
	buf[strcspn(buf, "\n")] = 0;
	return 0;
}

static int uio_name(const char *entry)
{
	char path[256], name[64];
	unsigned int index;
	char tail;

	if (sscanf(entry, "uio%u%c", &index, &tail) != 1)
		return 0;
	snprintf(path, sizeof(path), "/sys/class/uio/%s/name", entry);
	return !read_text(path, name, sizeof(name)) && !strcmp(name, "scu-mu2");
}

static int find_device(struct scu_mu *mu, const char *device, char path[64])
{
	struct dirent *entry;
	DIR *dir;
	unsigned int index, found = 0;
	char tail;

	if (device) {
		if (sscanf(device, "/dev/uio%u%c", &index, &tail) != 1 ||
		    strlen(device) >= 64 || !uio_name(device + 5))
			return fail(mu, "expected a /dev/uioN device named scu-mu2");
		snprintf(path, 64, "%s", device);
		return 0;
	}
	dir = opendir("/sys/class/uio");
	if (dir) {
		while ((entry = readdir(dir))) {
			if (!uio_name(entry->d_name))
				continue;
			snprintf(path, 64, "/dev/%.50s", entry->d_name);
			found++;
		}
		closedir(dir);
	}
	if (found > 1)
		return fail(mu, "multiple scu-mu2 devices; select one with --uio");
	if (!found)
		return fail(mu, "scu-mu2 UIO device missing; see docs/ahab-uio.md");
	return 0;
}

static int check_device(struct scu_mu *mu, const char *device)
{
	static const char *const attrs[] = { "addr", "size", "offset" };
	unsigned long long values[3];
	char path[256], text[256], tail;
	size_t i;
	ssize_t n;

	for (i = 0; i < ARRAY_SIZE(attrs); i++) {
		snprintf(path, sizeof(path), "/sys/class/uio/%s/maps/map0/%s",
			 device + 5, attrs[i]);
		if (read_text(path, text, sizeof(text)) ||
		    sscanf(text, "%llx%c", &values[i], &tail) != 1)
			return fail(mu, "cannot validate the UIO mapping");
	}
	if (values[0] != soc->scu_mu || values[1] != 0x10000 || values[2])
		return fail(mu, "UIO mapping does not match the supported MU2");
	snprintf(path, sizeof(path), "/sys/class/uio/%s/device/driver", device + 5);
	n = readlink(path, text, sizeof(text) - 1);
	if (n < 0)
		return fail(mu, "cannot identify the UIO driver");
	text[n] = 0;
	if (!strrchr(text, '/') ||
	    strcmp(strrchr(text, '/') + 1, "uio_pdrv_genirq"))
		return fail(mu, "MU2 must use uio_pdrv_genirq for power management");
	return 0;
}

static int lock_mu(struct scu_mu *mu)
{
	struct stat st;

	mu->lock = open(MU_LOCK, O_CREAT | O_RDWR | O_NOFOLLOW | O_CLOEXEC, 0600);
	if (mu->lock < 0 || fstat(mu->lock, &st) || !S_ISREG(st.st_mode) ||
	    st.st_nlink != 1 || st.st_uid != geteuid())
		return fail(mu, "cannot open the MU2 lock in /run");
	if (flock(mu->lock, LOCK_EX | LOCK_NB))
		return fail(mu, "MU2 is in use by another imx-sectool process");
	if (fstat(mu->lock, &st))
		return fail(mu, "cannot read the MU2 request state");
	if (st.st_size)
		return fail(mu, "previous MU2 request was incomplete; reboot before retrying");
	return 0;
}

void scu_close(struct scu_mu *mu)
{
	if (mu->regs)
		munmap((void *)mu->regs, MU_MAP_SIZE);
	if (mu->fd >= 0)
		close(mu->fd);
	if (mu->lock >= 0)
		close(mu->lock);
	mu->regs = NULL;
	mu->fd = mu->lock = -1;
}

int scu_open(struct scu_mu *mu, const char *device)
{
	char path[64], text[64];

	memset(mu, 0, sizeof(*mu));
	mu->fd = mu->lock = -1;
#if !defined(__linux__) || !defined(__aarch64__)
	(void)device;
	(void)path;
	(void)text;
	return fail(mu, "SCU UIO access requires Linux on arm64");
#endif
	if (!soc->scu_mu)
		return fail(mu, "no UIO mailbox verified on this SoC yet");
	if (read_text("/sys/devices/soc0/soc_id", text, sizeof(text)) ||
	    strcmp(text, "i.MX8QM"))
		return fail(mu, "the detected hardware is not i.MX8QM");
	if (find_device(mu, device, path) || check_device(mu, path) || lock_mu(mu))
		goto error;
	mu->fd = open(path, O_RDWR | O_SYNC | O_NOFOLLOW | O_CLOEXEC);
	if (mu->fd < 0) {
		fail(mu, "cannot open MU2 UIO device; check permissions and driver");
		goto error;
	}
	if (read_text("/sys/kernel/debug/pm_genpd/mu_a2/current_state",
		      text, sizeof(text)) || strcmp(text, "on")) {
		fail(mu, "MU2 power is off or unknown; check genpd debugfs access");
		goto error;
	}
	mu->regs = mmap(NULL, MU_MAP_SIZE, PROT_READ | PROT_WRITE,
			MAP_SHARED, mu->fd, 0);
	if (mu->regs == MAP_FAILED) {
		mu->regs = NULL;
		fail(mu, "cannot map MU2 through UIO");
		goto error;
	}
	return 0;
error:
	scu_close(mu);
	return -1;
}

static void barrier(void)
{
#if defined(__aarch64__)
	__asm__ volatile("dmb sy" ::: "memory");
#endif
}

static uint32_t read_reg(struct scu_mu *mu, unsigned int offset)
{
	uint32_t value;

	barrier();
	value = mu->regs[offset / 4];
	barrier();
	return value;
}

static void write_reg(struct scu_mu *mu, unsigned int offset, uint32_t value)
{
	barrier();
	mu->regs[offset / 4] = value;
	barrier();
}

static int64_t now_ms(void)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts))
		return -1;
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int wait_flag(struct scu_mu *mu, uint32_t mask, int64_t deadline)
{
	struct timespec delay = { .tv_nsec = 1000000 };
	int64_t now;

	while (!(read_reg(mu, MU_SR) & mask)) {
		now = now_ms();
		if (now < 0 || now >= deadline)
			return fail(mu, "MU2 request timed out; event list is incomplete");
		nanosleep(&delay, NULL);
	}
	return 0;
}

static int idle(struct scu_mu *mu)
{
	uint32_t sr = read_reg(mu, MU_SR);

	if (read_reg(mu, MU_CR) || (sr & 0x00f00000) != 0x00f00000 ||
	    (sr & 0xff000080))
		return fail(mu, "MU2 has pending traffic or interrupts enabled");
	return 0;
}

static int reply_status(struct scu_mu *mu, uint32_t header, unsigned int function)
{
	unsigned int count = (header >> 8) & 0xff;
	unsigned int error = header >> 24;

	if ((header & 0xff) != 1 || ((header >> 16) & 0xff) != 1)
		return fail(mu, "invalid SCU response header");
	if (!count || count > 8 ||
	    (!error && count != (function == 17 ? 4u : 2u)))
		return fail(mu, "unexpected SCU response length");
	if (function == 19 && error == 3 && count != 2)
		return fail(mu, "unexpected end-of-list response length");
	if (error)
		snprintf(mu->error, sizeof(mu->error), "SCFW returned error %u", error);
	return error;
}

int scu_call(struct scu_mu *mu, unsigned int function, unsigned int index,
	     uint32_t reply[8])
{
	uint32_t tx[2], header;
	unsigned int i, count, words;
	int status;
	int64_t deadline;

	if ((function != 17 && function != 19) || index > 255)
		return fail(mu, "unsupported SCU request");
	if (idle(mu))
		return -1;
	deadline = now_ms();
	if (deadline < 0)
		return fail(mu, "cannot read the monotonic clock");
	deadline += 2000;
	/* Keep this marker after a timeout or interrupted transfer. */
	if (pwrite(mu->lock, "incomplete\n", 11, 0) != 11)
		return fail(mu, "cannot record the MU2 request state");
	words = function == 17 ? 1 : 2;
	tx[0] = 1u | words << 8 | 9u << 16 | function << 24;
	tx[1] = index;
	for (i = 0; i < words; i++) {
		if (wait_flag(mu, 1u << (23 - i), deadline))
			return -1;
		write_reg(mu, i * 4, tx[i]);
	}
	if (wait_flag(mu, 1u << 27, deadline))
		return -1;
	header = read_reg(mu, 0x10);
	reply[0] = header;
	count = (header >> 8) & 0xff;
	if (!count || count > 8)
		return fail(mu, "invalid SCU response length");
	for (i = 1; i < count; i++) {
		if (wait_flag(mu, 1u << (27 - i % 4), deadline))
			return -1;
		reply[i] = read_reg(mu, 0x10 + (i % 4) * 4);
	}
	status = reply_status(mu, header, function);
	if (status < 0)
		return -1;
	if (idle(mu))
		return -1;
	if (ftruncate(mu->lock, 0))
		return fail(mu, "cannot clear the MU2 request marker");
	return status;
}
