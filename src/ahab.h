/* SPDX-License-Identifier: MIT */
/*
 * imx-sectool -- SECO status and SCU mailbox definitions.
 *
 * Author: Stefano Viola <stefano.viola@toradex.com>
 */
#ifndef AHAB_H
#define AHAB_H

#include "sectool.h"

#define AHAB_MAX_EVENTS 256
#define AHAB_ERROR_SIZE 160

struct scu_mu {
	int fd, lock;
	volatile uint32_t *regs;
	char error[AHAB_ERROR_SIZE];
};

struct ahab_info {
	int have_lifecycle, have_version;
	uint16_t lifecycle;
	uint32_t version;
	uint32_t events[AHAB_MAX_EVENTS];
	unsigned int count;
	int complete;
	char info_error[AHAB_ERROR_SIZE];
	char event_error[AHAB_ERROR_SIZE];
};

int scu_open(struct scu_mu *mu, const char *device);
void scu_close(struct scu_mu *mu);
int scu_call(struct scu_mu *mu, unsigned int function, unsigned int index,
	     uint32_t reply[8]);
int seco_info(struct ahab_info *info);
void ahab_read(struct ahab_info *info, const char *device);

#endif
