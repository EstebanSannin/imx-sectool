// SPDX-License-Identifier: MIT
/*
 * imx-sectool -- AHAB command tests with a fake transport.
 *
 * Author: Stefano Viola <stefano.viola@toradex.com>
 */
#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "ahab.h"

static const struct soc_desc test_soc = {
	.soc_id = "i.MX8QM", .seco = 1,
};
const struct soc_desc *soc = &test_soc;
int opt_json;
static const char *scenario;
static unsigned int requests, closes;

int seco_info(struct ahab_info *info)
{
	if (!strcmp(scenario, "fallback") || !strcmp(scenario, "unavailable")) {
		snprintf(info->info_error, sizeof(info->info_error), "%s", "SECO unavailable");
		return -1;
	}
	info->have_lifecycle = info->have_version = 1;
	info->lifecycle = 0x20;
	info->version = 0x30085;
	if (!strcmp(scenario, "close-error"))
		snprintf(info->info_error, sizeof(info->info_error), "%s", "session close failed");
	return 0;
}

int scu_open(struct scu_mu *mu, const char *device)
{
	(void)device;
	if (!strcmp(scenario, "missing") || !strcmp(scenario, "unavailable")) {
		snprintf(mu->error, sizeof(mu->error), "%s", "UIO missing");
		return -1;
	}
	return 0;
}

void scu_close(struct scu_mu *mu)
{
	(void)mu;
	closes++;
}

int scu_call(struct scu_mu *mu, unsigned int function, unsigned int index,
	     uint32_t reply[8])
{
	assert(requests++ <= 256);
	if (function == 17) {
		reply[3] = !strcmp(scenario, "mismatch") ? 0x80 : 0x20;
		return 0;
	}
	assert(function == 19);
	assert(index == requests - 2);
	if (!strcmp(scenario, "error")) {
		snprintf(mu->error, sizeof(mu->error), "%s", "SCFW returned error 7");
		return 7;
	}
	if (!strcmp(scenario, "timeout") || (!strcmp(scenario, "partial") && index)) {
		snprintf(mu->error, sizeof(mu->error), "%s", "request timed out: \"test\"\n");
		return -1;
	}
	if ((!strcmp(scenario, "events") && index < 2) ||
	    !strcmp(scenario, "limit") || !strcmp(scenario, "partial")) {
		reply[1] = index ? 0x0099ff00 : 0x0087ee00;
		return 0;
	}
	return 3;
}

int main(int argc, char **argv)
{
	char command[] = "ahab_status", option[] = "--json";
	char *args[2];
	int rc;

	assert(argc == 2);
	scenario = argv[1];
	args[0] = command;
	args[1] = option;
	rc = cmd_ahab_status(2, args);
	if (!strcmp(scenario, "missing") || !strcmp(scenario, "unavailable"))
		assert(!requests && !closes);
	else
		assert(closes == 1);
	return rc;
}
