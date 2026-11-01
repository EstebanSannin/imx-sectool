// SPDX-License-Identifier: MIT
/*
 * imx-sectool -- the U-Boot fuse / hab_status commands and boot-time security
 * information for NXP i.MX, from Linux userspace.
 *
 * Multi-call binary: invoked as 'fuse' or 'hab_status' it behaves like the
 * U-Boot command of that name; as 'imx-sectool <command>' it offers all
 * commands plus global options. See DESIGN.md for the full specification.
 *
 * Data sources, in order of preference: nvmem for fuses; read-only, whitelisted
 * /dev/mem mappings for ROM, OCRAM, SRC and SNVS. No device-tree change and no
 * kernel module are required for HAB. AHAB events use optional UIO setup.
 *
 * Environment (mainly for host testing):
 *   IMX_SECTOOL_SOC       SoC id instead of /sys/devices/soc0/soc_id
 *   IMX_SECTOOL_NVMEM     nvmem file instead of the detected OCOTP device
 *   IMX_SECTOOL_MEM_DUMP  directory of <region>.bin files instead of /dev/mem
 *
 * Deps: libc only.  License: MIT.
 *
 * Author: Stefano Viola <stefano.viola@toradex.com>
 */
#define _POSIX_C_SOURCE 200809L
#include <stdlib.h>
#include <string.h>
#include <libgen.h>

#include "sectool.h"

int opt_json;
const char *opt_nvmem;

struct command {
	const char *name;
	int (*fn)(int argc, char **argv);
	const char *help;
};

static const struct command commands[] = {
	{ "fuse", cmd_fuse,
	  "U-Boot 'fuse' command (read, readv, readm, cmp, prog)" },
	{ "hab_status", cmd_hab_status,
	  "U-Boot 'hab_status': secure boot state and HAB events" },
	{ "ahab_status", cmd_ahab_status,
	  "SECO lifecycle and AHAB events [--uio /dev/uioN]" },
	{ "romlog", cmd_romlog, "decode the i.MX8M Boot ROM event log" },
	{ "reset-cause", cmd_reset_cause,
	  "reset cause from the System Reset Controller" },
	{ "debug-status", cmd_debug_status,
	  "JTAG / secure-debug policy [--expect <mode>] [--profile <file>]" },
	{ "audit", cmd_audit,
	  "secure-boot and secure-debug checklist [--profile <file>]" },
	{ "fusemap", cmd_fusemap,
	  "named fuses decoded [<FIELD|REGISTER|SRK_HASH|MAC0|MAC1>]" },
	{ "bootinfo", cmd_bootinfo,
	  "how this boot happened: boot mode, image, authentication" },
};
#define N_COMMANDS (ARRAY_SIZE(commands))

static void usage(FILE *f)
{
	size_t i;

	fprintf(f,
		"usage: imx-sectool [--json] [--soc <id>] [--nvmem <file>] <command> [args]\n"
		"       imx-sectool --version | --help\n\ncommands:\n");
	for (i = 0; i < N_COMMANDS; i++)
		fprintf(f, "  %-13s %s\n", commands[i].name, commands[i].help);
	fprintf(f,
		"\nAlso installed as 'fuse' and 'hab_status', it behaves like those U-Boot commands.\n");
}

static int run_command(const struct command *cmd, int argc, char **argv)
{
	if (soc->seco && cmd->fn != cmd_ahab_status) {
		fprintf(stderr, "error: only ahab_status is supported on %s yet\n",
			soc->name);
		return EX_UNAVAIL;
	}
	return cmd->fn(argc, argv);
}

static const struct command *find_command(const char *name)
{
	size_t i;

	for (i = 0; i < N_COMMANDS; i++)
		if (!strcmp(commands[i].name, name))
			return &commands[i];
	return NULL;
}

int main(int argc, char **argv)
{
	const char *force_soc = getenv("IMX_SECTOOL_SOC");
	const struct command *cmd;
	int ai, i, rc;

	opt_nvmem = getenv("IMX_SECTOOL_NVMEM");

	/* busybox-style: 'fuse ...' and 'hab_status' keep U-Boot's exact syntax */
	cmd = find_command(basename(argv[0]));
	if (cmd) {
		rc = soc_detect(force_soc);
		if (rc != EX_OK)
			return rc;
		return run_command(cmd, argc, argv);
	}

	for (ai = 1; ai < argc; ai++) {
		if (!strcmp(argv[ai], "--version")) {
			printf("imx-sectool %s\n", VERSION);
			return EX_OK;
		}
		if (!strcmp(argv[ai], "-h") || !strcmp(argv[ai], "--help")) {
			usage(stdout);
			return EX_OK;
		}
		if (!strcmp(argv[ai], "--json"))
			opt_json = 1;
		else if (!strcmp(argv[ai], "--soc") && ai + 1 < argc)
			force_soc = argv[++ai];
		else if (!strcmp(argv[ai], "--nvmem") && ai + 1 < argc)
			opt_nvmem = argv[++ai];
		else
			break;
	}
	if (ai >= argc) {
		usage(stderr);
		return EX_USAGE;
	}

	cmd = find_command(argv[ai]);
	if (!cmd) {
		fprintf(stderr, "error: unknown command '%s'\n\n", argv[ai]);
		usage(stderr);
		return EX_USAGE;
	}

	/* accept --json after the command too */
	for (i = ai + 1; i < argc; i++)
		if (!strcmp(argv[i], "--json"))
			opt_json = 1;

	rc = soc_detect(force_soc);
	if (rc != EX_OK)
		return rc;
	return run_command(cmd, argc - ai, &argv[ai]);
}
