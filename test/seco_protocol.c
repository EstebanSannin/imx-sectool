// SPDX-License-Identifier: MIT
/*
 * imx-sectool -- SECO response validation tests with a fake ioctl.
 *
 * Author: Stefano Viola <stefano.viola@toradex.com>
 */
#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#include <sys/ioctl.h>
#include <stdarg.h>
#include <assert.h>

static int mock_ioctl(int fd, int request, ...);
#define ioctl mock_ioctl
#include "../src/seco.c"
#undef ioctl

const struct soc_desc *soc;
static int scenario;

static int mock_ioctl(int fd, int request, ...)
{
	struct se_transfer *transfer;
	unsigned int i;
	va_list args;

	(void)fd;
	assert(request == (int)SE_TRANSFER);
	va_start(args, request);
	transfer = va_arg(args, struct se_transfer *);
	va_end(args);
	assert(transfer->tx[0] == 0x17160206);
	assert(transfer->tx_size == 8 && transfer->rx_size == 40);
	if (scenario == 1)
		return -1;
	transfer->rx[0] = 0xe1160a06;
	if (scenario == 2)
		transfer->rx[0] = 0xe1150a06;
	if (scenario == 3)
		transfer->rx[0] = 0xe1160b06;
	if (scenario == 4)
		transfer->rx[0] = 0xe1160106;
	if (scenario == 5) {
		transfer->rx[0] = 0xe1160206;
		transfer->rx[1] = 0x429;
		return 0;
	}
	for (i = 0; i < 9; i++)
		transfer->rx[9] ^= transfer->rx[i];
	if (scenario == 6)
		transfer->rx[9] ^= 1;
	return 0;
}

int main(void)
{
	struct se_mu_info mu = { .cmd_tag = 0x17, .rsp_tag = 0xe1 };
	uint32_t tx[2] = { 0 }, rx[10];
	char error[AHAB_ERROR_SIZE];
	int rc;

	for (scenario = 0; scenario <= 6; scenario++) {
		rc = se_call(0, &mu, 0x16, tx, 2, rx, 10, error);
		if (scenario == 0)
			assert(rc == 0);
		else if (scenario == 5)
			assert(rc == 0x429 + 1);
		else
			assert(rc == -1);
	}
	puts("SECO response validation tests passed");
	return 0;
}
