// SPDX-License-Identifier: MIT
/*
 * imx-sectool -- 'ahab_status': SECO lifecycle and authentication events.
 *
 * Lifecycle and firmware information come from the NXP fsl-se driver.
 * Events use an optional SCU mailbox exposed through UIO. Missing or
 * incomplete events are reported as unavailable, never as a clean result.
 *
 * Author: Stefano Viola <stefano.viola@toradex.com>
 */
#include <string.h>

#include "ahab.h"

static const char *lifecycle_name(uint16_t lifecycle)
{
	switch (lifecycle) {
	case 0x001:
		return "Pristine";
	case 0x002:
		return "Fab";
	case 0x008:
		return "Open";
	case 0x020:
		return "NXP closed";
	case 0x080:
		return "OEM closed";
	case 0x100:
		return "Partial field return";
	case 0x200:
		return "Full field return";
	case 0x400:
		return "No return";
	default:
		return "Unknown";
	}
}

static const char *event_command(uint32_t event)
{
	switch ((event >> 16) & 0xff) {
	case 0x87:
		return "AUTH_CONTAINER_REQ";
	case 0x88:
		return "VERIFY_IMAGE_REQ";
	default:
		return "Unknown";
	}
}

static const char *event_indicator(uint32_t event)
{
	if (((event >> 16) & 0xff) != 0x87 && ((event >> 16) & 0xff) != 0x88)
		return "Unknown";
	switch ((event >> 8) & 0xff) {
	case 0xee:
		return "NO_AUTHENTICATION";
	case 0xfa:
		return "BAD_KEY_HASH";
	case 0xf9:
		return "INVALID_KEY";
	case 0xf0:
		return "BAD_SIGNATURE";
	case 0xf1:
		return "BAD_HASH";
	default:
		return "Unknown";
	}
}

void ahab_read(struct ahab_info *info, const char *device)
{
	struct scu_mu mu;
	uint32_t reply[8];
	unsigned int index;
	int rc;

	memset(info, 0, sizeof(*info));
	seco_info(info);
	if (scu_open(&mu, device))
		goto unavailable;
	rc = scu_call(&mu, 17, 0, reply);
	if (rc)
		goto close;
	if (!info->have_lifecycle) {
		info->lifecycle = reply[3] & 0xffff;
		info->have_lifecycle = 1;
		info->info_error[0] = 0;
	} else if (info->lifecycle != (reply[3] & 0xffff)) {
		snprintf(mu.error, sizeof(mu.error), "SECO and SCU lifecycle values differ");
		goto close;
	}
	for (index = 0; index < AHAB_MAX_EVENTS; index++) {
		rc = scu_call(&mu, 19, index, reply);
		/* SCFW returns SC_ERR_PARM after the last event. */
		if (rc == 3) {
			info->complete = 1;
			break;
		}
		if (rc)
			goto close;
		info->events[info->count++] = reply[1];
	}
	if (!info->complete)
		snprintf(mu.error, sizeof(mu.error), "event index limit reached; list is incomplete");
close:
	scu_close(&mu);
unavailable:
	if (!info->complete)
		snprintf(info->event_error, sizeof(info->event_error), "%s", mu.error);
}

static void json_string(const char *s)
{
	unsigned char c;

	putchar('"');
	while ((c = (unsigned char)*s++)) {
		if (c == '"' || c == '\\')
			printf("\\%c", c);
		else if (c < 0x20)
			printf("\\u%04x", c);
		else
			putchar(c);
	}
	putchar('"');
}

static void print_json(const struct ahab_info *info)
{
	unsigned int i;

	printf("{\"soc\": \"%s\", \"lifecycle\": ", soc->soc_id);
	if (info->have_lifecycle)
		printf("{\"value\": %u, \"name\": \"%s\"}", info->lifecycle,
		       lifecycle_name(info->lifecycle));
	else
		printf("null");
	printf(", \"firmware_version\": ");
	if (info->have_version)
		printf("%u", info->version);
	else
		printf("null");
	printf(", \"events_complete\": %s, \"events\": [",
	       info->complete ? "true" : "false");
	for (i = 0; i < info->count; i++)
		printf("%s{\"index\": %u, \"value\": %u, \"command\": \"%s\", \"indicator\": \"%s\"}", i ? ", " : "", i, info->events[i],
		       event_command(info->events[i]), event_indicator(info->events[i]));
	printf("], \"info_error\": ");
	json_string(info->info_error);
	printf(", \"event_error\": ");
	json_string(info->event_error);
	printf("}\n");
}

static void print_text(const struct ahab_info *info)
{
	unsigned int i;

	if (info->have_lifecycle)
		printf("Lifecycle:   0x%04x (%s)\n", info->lifecycle,
		       lifecycle_name(info->lifecycle));
	else
		printf("Lifecycle:   unavailable\n");
	if (info->have_version)
		printf("SECO version: 0x%08x\n", info->version);
	if (info->info_error[0])
		printf("SECO info:   %s\n", info->info_error);
	if (!info->complete)
		printf("AHAB events: %s\n  %s\n", info->count ? "incomplete" : "unavailable",
		       info->event_error);
	else
		printf("AHAB events: %u\n", info->count);
	for (i = 0; i < info->count; i++)
		printf("  Event[%u]: 0x%08x (%s / %s)\n", i, info->events[i],
		       event_command(info->events[i]), event_indicator(info->events[i]));
}

int cmd_ahab_status(int argc, char **argv)
{
	struct ahab_info info;
	const char *device = NULL;
	int i;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--json")) {
			opt_json = 1;
			continue;
		}
		if (!strcmp(argv[i], "--uio") && i + 1 < argc && !device)
			device = argv[++i];
		else {
			fprintf(stderr, "usage: ahab_status [--json] [--uio /dev/uioN]\n");
			return EX_USAGE;
		}
	}
	if (!soc->seco) {
		fprintf(stderr, "error: ahab_status requires an SCU/SECO SoC\n");
		return EX_UNAVAIL;
	}
	ahab_read(&info, device);
	if (opt_json)
		print_json(&info);
	else
		print_text(&info);
	if (!info.complete || !info.have_lifecycle || info.info_error[0])
		return EX_UNAVAIL;
	return info.count ? EX_FAIL : EX_OK;
}
