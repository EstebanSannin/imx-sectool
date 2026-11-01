// SPDX-License-Identifier: MIT
/*
 * imx-sectool -- lifecycle and firmware information through NXP fsl-se.
 *
 * Opens an HSM session, reads GET_INFO and closes the session. Responses
 * are checked before use. No lifecycle changes or fuse writes are made.
 *
 * Author: Stefano Viola <stefano.viola@toradex.com>
 */
#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#include <fcntl.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "ahab.h"

struct se_mu_info {
	uint8_t id, irq, tz, did, cmd_tag, rsp_tag, success, base_ver, fw_ver;
};

struct se_transfer {
	uint32_t *tx;
	int tx_size;
	uint32_t *rx;
	int rx_size;
};

#define SE_GET_MU_INFO _IOR(0x0a, 0x04, struct se_mu_info)
#define SE_TRANSFER _IOWR(0x0a, 0x07, struct se_transfer)

static int se_call(int fd, const struct se_mu_info *mu, unsigned int command,
		   uint32_t *tx, unsigned int ntx, uint32_t *rx,
		   unsigned int nrx, char *error)
{
	struct se_transfer transfer = { tx, ntx * 4, rx, nrx * 4 };
	unsigned int i, count;
	uint32_t crc = 0, header;

	tx[0] = 6u | ntx << 8 | command << 16 | (uint32_t)mu->cmd_tag << 24;
	memset(rx, 0, nrx * 4);
	if (ioctl(fd, (int)SE_TRANSFER, &transfer) < 0) {
		snprintf(error, AHAB_ERROR_SIZE, "SECO ioctl failed for command 0x%02x",
			 command);
		return -1;
	}
	header = rx[0];
	count = (header >> 8) & 0xff;
	if ((header & 0xff) != 6 || ((header >> 16) & 0xff) != command ||
	    (header >> 24) != mu->rsp_tag || count < 2 || count > nrx)
		goto malformed;
	if (count >= 4) {
		for (i = 0; i < count; i++)
			crc ^= rx[i];
		if (crc)
			goto malformed;
	}
	if ((rx[1] & 0xff) != mu->success) {
		snprintf(error, AHAB_ERROR_SIZE, "SECO command 0x%02x returned 0x%08x",
			 command, rx[1]);
		return (int)(rx[1] & 0xffff) + 1;
	}
	if (count != nrx)
		goto malformed;
	return 0;
malformed:
	snprintf(error, AHAB_ERROR_SIZE, "invalid SECO response for command 0x%02x",
		 command);
	return -1;
}

int seco_info(struct ahab_info *info)
{
	struct se_mu_info mu;
	uint32_t tx[3] = { 0 }, rx[10], session;
	char close_error[AHAB_ERROR_SIZE];
	int fd, rc, close_rc;

#if !defined(__linux__) || !defined(__aarch64__)
	snprintf(info->info_error, sizeof(info->info_error),
		 "SECO access requires Linux on arm64");
	return -1;
#endif
	fd = open("/dev/hsm0_ch0", O_RDWR | O_CLOEXEC | O_NOFOLLOW);
	if (fd < 0) {
		snprintf(info->info_error, sizeof(info->info_error),
			 "cannot open /dev/hsm0_ch0; check driver and permissions");
		return -1;
	}
	if (ioctl(fd, (int)SE_GET_MU_INFO, &mu) < 0) {
		snprintf(info->info_error, sizeof(info->info_error),
			 "SECO interface information is unavailable");
		rc = -1;
		goto out;
	}
	tx[1] = mu.id | (uint32_t)mu.irq << 8 | (uint32_t)mu.tz << 16 |
		(uint32_t)mu.did << 24;
	rc = se_call(fd, &mu, 0x10, tx, 3, rx, 3, info->info_error);
	/* Some i.MX8QM drivers report HSM interface 0 instead of 2. */
	if (rc == 0x429 + 1 && mu.id == 0 && !strcmp(soc->soc_id, "i.MX8QM")) {
		tx[1] = (tx[1] & ~0xffu) | 2;
		rc = se_call(fd, &mu, 0x10, tx, 3, rx, 3, info->info_error);
	}
	if (rc)
		goto out;
	session = rx[2];
	tx[1] = session;
	rc = se_call(fd, &mu, 0x16, tx, 2, rx, 10, info->info_error);
	if (!rc) {
		info->lifecycle = rx[5] >> 16;
		info->version = rx[6];
		info->have_lifecycle = info->have_version = 1;
		info->info_error[0] = 0;
	}
	tx[1] = session;
	close_rc = se_call(fd, &mu, 0x11, tx, 2, rx, 2, close_error);
	if (close_rc) {
		snprintf(info->info_error, sizeof(info->info_error), "%s", close_error);
		rc = -1;
	}
out:
	close(fd);
	return rc;
}
