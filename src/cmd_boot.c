// SPDX-License-Identifier: MIT
/*
 * imx-sectool -- boot-time information: 'romlog' and 'reset-cause'.
 *
 * romlog       The i.MX8M Boot ROM keeps an event log in OCRAM; a pointer to
 *              it sits at a fixed ROM address (0x9e0). Event IDs and their
 *              parameter counts follow U-Boot's
 *              imx8m_detect_secondary_image_boot() and NXP AN12853; IDs not
 *              documented there are printed raw. Event 0x51 means the ROM fell
 *              back to the secondary boot image.
 *
 * reset-cause  SRC_SRSR, decoded with U-Boot's strings. Some bootloaders
 *              clear SRSR after reading it; then nothing is left to report.
 *
 * Author: Stefano Viola <stefano.viola@toradex.com>
 */
#include <string.h>

#include "sectool.h"

#define ROM_LOG_MAX_WORDS 128
#define SRC_SRSR 0x5c

/* ---- ROM log -------------------------------------------------------------- */

struct rom_event {
	uint8_t id;
	int nparams;
	const char *name;
	const char *p0, *p1; /* parameter labels */
};

static const struct rom_event rom_events[] = {
	{ 0x50, 0, "boot from primary image", NULL, NULL },
	{ 0x51, 0, "boot from SECONDARY image", NULL, NULL },
	{ 0x80, 1, "device init start", "ts", NULL },
	{ 0x81, 1, "device init done", "ts", NULL },
	{ 0x82, 1, "device driver pre-config", "ts", NULL },
	{ 0x8f, 1, "device init FAILED", "ts", NULL },
	{ 0x90, 1, "read start", "arg", NULL },
	{ 0x91, 1, "read done", "ts", NULL },
	{ 0x9f, 1, "read FAILED", "ts", NULL },
	{ 0xa0, 2, "image authentication result", "result", "ts" },
	{ 0xc0, 2, "jump to boot image", "addr", "ts" },
};

static const struct rom_event *rom_event_find(uint8_t id)
{
	size_t i;

	for (i = 0; i < ARRAY_SIZE(rom_events); i++)
		if (rom_events[i].id == id)
			return &rom_events[i];
	return NULL;
}

int cmd_romlog(int argc, char **argv)
{
	uint32_t ptr, log[ROM_LOG_MAX_WORDS];
	int secondary = 0, ended = 0;
	const struct rom_event *e;
	int i, p, np;
	uint8_t id;
	uint32_t w;

	(void)argc;
	(void)argv;

	if (soc->no_rom_log) {
		fprintf(stderr, "error: the %s Boot ROM keeps no ROM log\n",
			soc->name);
		return EX_UNAVAIL;
	}
	if (mmio_read32(soc->rom_log_ptr, &ptr)) {
		fprintf(stderr, "error: ROM log unavailable: %s\n",
			mmio_error());
		return EX_UNAVAIL;
	}
	if (ptr < soc->rom_log_lo || ptr >= soc->rom_log_hi || (ptr & 3)) {
		fprintf(stderr,
			"error: ROM log pointer 0x%08x at 0x%x is not valid\n",
			ptr, soc->rom_log_ptr);
		return EX_UNTRUSTED;
	}
	if (mmio_read(ptr, log, sizeof(log))) {
		fprintf(stderr, "error: ROM log at 0x%08x unavailable: %s\n",
			ptr, mmio_error());
		return EX_UNAVAIL;
	}

	if (opt_json)
		printf("{\"address\": \"0x%08x\", \"events\": [", ptr);
	else
		printf("ROM log at 0x%08x:\n", ptr);

	for (i = 0; i < ROM_LOG_MAX_WORDS; i++) {
		w = log[i];
		id = w >> 24;
		if (id == 0x00) {
			ended = 1;
			break;
		}
		if (id == 0x51)
			secondary = 1;

		e = rom_event_find(id);
		np = e ? e->nparams : 0;
		if (i + np >= ROM_LOG_MAX_WORDS)
			np = ROM_LOG_MAX_WORDS - 1 - i;

		if (opt_json) {
			printf("%s\n  {\"raw\": \"0x%08x\", \"id\": \"0x%02x\", \"name\": ",
			       i ? "," : "", w, id);
			if (e)
				printf("\"%s\"", e->name);
			else
				printf("null");
			printf(", \"params\": [");
			for (p = 0; p < np; p++)
				printf("%s\"0x%08x\"", p ? ", " : "",
				       log[i + 1 + p]);
			printf("]}");
		} else {
			printf("  %08x  ", w);
			/* pad the name only when parameters follow it */
			if (!e)
				printf("event 0x%02x (arg 0x%06x)", id,
				       w & 0xffffff);
			else if (np)
				printf("%-28s", e->name);
			else
				printf("%s", e->name);
			if (np >= 1)
				printf(" %s=0x%08x", e->p0, log[i + 1]);
			if (np >= 2)
				printf(" %s=0x%08x", e->p1, log[i + 2]);
			putchar('\n');
		}
		i += np;
	}

	if (opt_json) {
		printf("\n], \"complete\": %s, \"boot_image\": \"%s\"}\n",
		       ended ? "true" : "false",
		       secondary ? "secondary" : "primary");
	} else {
		if (!ended)
			printf("  (no end marker within %d words)\n",
			       ROM_LOG_MAX_WORDS);
		printf("Boot image: %s\n",
		       secondary ? "SECONDARY (ROM fell back)" : "primary");
	}
	return EX_OK;
}

/* ---- reset cause ------------------------------------------------------------ */

/* SRSR bits with U-Boot's names (arch/arm/mach-imx/cpu.c, i.MX8M; the
 * i.MX7 calls 0x10 WDOG1 and 0x100 WDOG4, see srsr_name())
 */
static const struct {
	uint32_t bit;
	const char *name;
} srsr_bits[] = {
	{ 0x00001, "POR" },	    { 0x00004, "CSU" },
	{ 0x00008, "IPP USER" },    { 0x00010, "WDOG" },
	{ 0x00020, "JTAG HIGH-Z" }, { 0x00040, "JTAG SW" },
	{ 0x00080, "WDOG3" },	    { 0x00100, "WDOG2" },
	{ 0x00200, "TEMPSENSE" },
};
#define N_SRSR_BITS (ARRAY_SIZE(srsr_bits))

static const char *srsr_name(size_t i)
{
	if (!strncmp(soc->soc_id, "i.MX7", 5)) {
		if (srsr_bits[i].bit == 0x10)
			return "WDOG1";
		if (srsr_bits[i].bit == 0x100)
			return "WDOG4";
	}
	return srsr_bits[i].name;
}

int cmd_reset_cause(int argc, char **argv)
{
	uint32_t srsr, known = 0;
	int first = 1;
	size_t i;

	(void)argc;
	(void)argv;

	if (mmio_read32(soc->src_base + SRC_SRSR, &srsr)) {
		fprintf(stderr, "error: reset cause unavailable: %s\n",
			mmio_error());
		return EX_UNAVAIL;
	}

	if (opt_json) {
		printf("{\"srsr\": \"0x%08x\", \"causes\": [", srsr);
		for (i = 0; i < N_SRSR_BITS; i++)
			if (srsr & srsr_bits[i].bit) {
				printf("%s\"%s\"", first ? "" : ", ",
				       srsr_name(i));
				first = 0;
			}
		printf("], \"reliable\": %s}\n",
		       soc->srsr_reliable ? "true" : "false");
		if (!srsr)
			return EX_UNAVAIL;
		return soc->srsr_reliable ? EX_OK : EX_WARN;
	}

	if (!srsr) {
		printf("Reset cause: unknown (SRC_SRSR is 0: the bootloader may have cleared it)\n");
		return EX_UNAVAIL;
	}
	printf("Reset cause: ");
	for (i = 0; i < N_SRSR_BITS; i++) {
		known |= srsr_bits[i].bit;
		if (srsr & srsr_bits[i].bit) {
			printf("%s%s", first ? "" : " + ", srsr_name(i));
			first = 0;
		}
	}
	if (srsr & ~known)
		printf("%sunknown bits 0x%x", first ? "" : " + ",
		       srsr & ~known);
	printf("  (SRC_SRSR 0x%08x)\n", srsr);
	if (!soc->srsr_reliable) {
		if (!strcmp(soc->soc_id, "i.MX7D")) {
			puts("note: U-Boot normally clears SRC_SRSR on i.MX7; this value may not describe the boot reset.");
			return EX_WARN;
		}
		printf("note: on %s, a power-on and a reboot leave the same SRC_SRSR value by the\n"
		       "      time Linux runs, so this cannot tell them apart.\n",
		       soc->name);
		return EX_WARN;
	}
	return EX_OK;
}

/* ---- ROM log summary (shared with bootinfo) ----------------------------------- */

int romlog_summary(struct romlog_info *ri, const char **why)
{
	uint32_t ptr, log[ROM_LOG_MAX_WORDS];
	const struct rom_event *e;
	int i, np;
	uint8_t id;

	memset(ri, 0, sizeof(*ri));
	if (soc->no_rom_log) {
		*why = "this Boot ROM keeps no ROM log";
		return -1;
	}
	if (mmio_read32(soc->rom_log_ptr, &ptr)) {
		*why = mmio_error();
		return -1;
	}
	if (ptr < soc->rom_log_lo || ptr >= soc->rom_log_hi || (ptr & 3)) {
		*why = "ROM log pointer is not valid";
		return -1;
	}
	if (mmio_read(ptr, log, sizeof(log))) {
		*why = mmio_error();
		return -1;
	}

	ri->addr = ptr;
	for (i = 0; i < ROM_LOG_MAX_WORDS; i++) {
		id = log[i] >> 24;
		if (id == 0x00)
			break;
		e = rom_event_find(id);
		np = e ? e->nparams : 0;
		if (i + np >= ROM_LOG_MAX_WORDS)
			break;
		if (id == 0x51)
			ri->secondary = 1;
		if (id == 0xa0 && !ri->have_auth) {
			ri->have_auth = 1;
			ri->auth = log[i + 1];
		}
		if (id == 0xc0 && !ri->have_jump) {
			ri->have_jump = 1;
			ri->jump = log[i + 1];
		}
		i += np;
	}
	return 0;
}

/* ---- bootinfo ------------------------------------------------------------------- */

#define SRC_SBMR1 0x58
#define SRC_SBMR2 0x70

/* Follow ROM_SW_INFO_ADDR, as U-Boot get_boot_device() does on i.MX7. */
static const char *boot_device_7d(unsigned int *instance, const char **why)
{
	static const char *const names[] = {
		[1] = "SD", [2] = "MMC", [3] = "NAND", [4] = "QSPI",
		[5] = "WEIM NOR", [6] = "SPI NOR", [15] = "USB",
	};
	uint32_t ptr, info;
	unsigned int type, port;

	if (mmio_read32(0x000001e8, &ptr)) {
		*why = mmio_error();
		return NULL;
	}
	if ((ptr & 3) || ptr < 0x00900000 || ptr > 0x00920000 - 32) {
		*why = "ROM software-info pointer is outside OCRAM or unaligned";
		return NULL;
	}
	if (mmio_read32(ptr, &info)) {
		*why = mmio_error();
		return NULL;
	}
	type = (info >> 16) & 0xff;
	port = (info >> 8) & 0xff;
	if (type >= ARRAY_SIZE(names) || !names[type]) {
		*why = "unknown device in ROM software info";
		return NULL;
	}
	if ((type == 1 || type == 2) && port >= 3) {
		*why = "invalid SD/MMC instance in ROM software info";
		return NULL;
	}
	*instance = type == 1 || type == 2 ? port + 1 : 0;
	return names[type];
}

/* U-Boot boot_mode_getprisec() reads this selection, not a boot result. */
static int bootinfo_7d(void)
{
	uint32_t gpr10;
	const char *why = NULL;
	const char *device;
	unsigned int instance = 0;
	int have_gpr, events;

	have_gpr = !mmio_read32(soc->src_base + 0x98, &gpr10);
	if (opt_json) {
		if (have_gpr)
			printf(", \"src_gpr10\": \"0x%08x\", \"boot_selection\": \"%s\"",
			       gpr10, gpr10 & (1u << 30) ? "secondary" : "primary");
		else
			printf(", \"boot_selection\": null");
	} else if (have_gpr) {
		printf("  Boot selection:         %s (SRC_GPR10 0x%08x; not a boot result)\n",
		       gpr10 & (1u << 30) ? "secondary" : "primary", gpr10);
	} else {
		printf("  Boot selection:         unavailable (%s)\n", mmio_error());
	}

	device = boot_device_7d(&instance, &why);
	if (opt_json) {
		if (device) {
			printf(", \"boot_device\": \"%s\"", device);
			if (instance)
				printf(", \"boot_instance\": %u", instance);
		} else {
			printf(", \"boot_device\": null");
		}
	} else if (device) {
		printf("  Boot device:            %s", device);
		if (instance)
			printf("%u", instance);
		puts(" (ROM software info; controller numbering)");
	} else {
		printf("  Boot device:            unavailable (%s)\n", why);
	}

	events = hab_event_count(&why);
	if (opt_json) {
		if (events >= 0)
			printf(", \"hab_events\": %d", events);
		else
			printf(", \"hab_events\": null");
		printf(", \"boot_image\": null, \"auth_result\": null");
	} else {
		if (events >= 0)
			printf("  HAB events:             %d (run hab_status for details)\n", events);
		else
			printf("  HAB events:             unavailable (%s)\n", why);
		puts("  Boot image:             unavailable (selection is not a boot result)");
		puts("  Image authentication:   unavailable (HAB events alone do not prove success)");
	}
	return have_gpr && device && events >= 0 ? EX_OK : EX_UNAVAIL;
}

/*
 * What can be stated about this boot without the ROM API (which only the
 * bootloader can call): SRC boot mode registers raw, boot-config source,
 * primary vs secondary image and the authentication result from the ROM log,
 * the secondary image offset (U-Boot's IMG_CNTN_SET1_OFFSET rule) and the
 * secure boot fuse. The boot device itself is not decoded: on i.MX8M Plus
 * U-Boot gets it from the ROM API, not from these registers.
 */
int cmd_bootinfo(int argc, char **argv)
{
	uint32_t sbmr1, sbmr2, sel = 0, off = 0, sec = 0;
	int have_src, have_log, have_sel, have_off, have_sec;
	const char *s, *r, *c, *offs, *src_why;
	uint8_t a_sts, a_rsn, a_ctx;
	const struct fuse_field *of;
	const char *why = NULL;
	struct romlog_info ri;
	char ob[64] = "";
	int rc = EX_OK;

	(void)argc;
	(void)argv;

	have_src = !mmio_read32(soc->src_base + SRC_SBMR1, &sbmr1) &&
		   !mmio_read32(soc->src_base + SRC_SBMR2, &sbmr2);
	src_why = have_src ? NULL : mmio_error();
	have_log = romlog_summary(&ri, &why) == 0;
	have_sel = field_read("BT_FUSE_SEL", &sel) == 0;
	have_sec = field_read("SEC_CONFIG[1]", &sec) == 0;
	/* only i.MX8MN/MP have this fuse */
	of = field_find("IMG_CNTN_SET1_OFFSET");
	have_off = !of || field_read(of->name, &off) == 0;

	if (!have_src || (!have_log && !soc->no_rom_log) || !have_sel ||
	    !have_off || !have_sec)
		rc = EX_UNAVAIL;

	offs = have_off && of ? field_meaning(of, off, ob, sizeof(ob)) : NULL;
	a_sts = ri.auth & 0xff;
	a_rsn = (ri.auth >> 8) & 0xff;
	a_ctx = (ri.auth >> 16) & 0xff;

	if (opt_json) {
		printf("{\"soc\": \"%s\"", soc->soc_id);
		if (have_src)
			printf(", \"sbmr1\": \"0x%08x\", \"sbmr2\": \"0x%08x\", \"bmod\": %u",
			       sbmr1, sbmr2, (sbmr2 >> 24) & 3);
		if (have_sel)
			printf(", \"boot_config_source\": \"%s\"",
			       sel ? "fuses" : "pins");
		if (have_log) {
			printf(", \"boot_image\": \"%s\"",
			       ri.secondary ? "secondary" : "primary");
			if (ri.have_auth)
				printf(", \"auth_result\": \"0x%08x\"",
				       ri.auth);
			if (ri.have_jump)
				printf(", \"entry\": \"0x%08x\"", ri.jump);
		}
		if (offs)
			printf(", \"secondary_image\": \"%s\"", offs);
		if (have_sec)
			printf(", \"secure_boot\": \"%s\"",
			       sec ? "closed" : "open");
		if (!strcmp(soc->soc_id, "i.MX7D") && bootinfo_7d())
			rc = EX_UNAVAIL;
		printf("}\n");
		return rc;
	}

	printf("Boot information - %s\n", soc->name);
	if (have_src) {
		printf("  SRC_SBMR1:              0x%08x  (boot config)\n",
		       sbmr1);
		printf("  SRC_SBMR2:              0x%08x  (BMOD %u%u)\n", sbmr2,
		       (sbmr2 >> 25) & 1, (sbmr2 >> 24) & 1);
	} else {
		printf("  SRC boot mode:          unavailable (%s)\n", src_why);
	}
	if (have_sel)
		printf("  Boot config source:     %s (BT_FUSE_SEL=%u)\n",
		       sel ? "fuses" : "boot pins", sel);
	if (have_log) {
		printf("  Boot image:             %s  (ROM log)\n",
		       ri.secondary ? "SECONDARY - the ROM fell back" :
				      "primary");
		if (ri.have_auth) {
			s = hab_code_name('s', a_sts);
			r = hab_code_name('r', a_rsn);
			c = hab_code_name('c', a_ctx);
			printf("  Image authentication:   0x%08x", ri.auth);
			if (s && r && c)
				printf("  (%s, %s, %s)", s, r, c);
			printf("\n");
		}
		if (ri.have_jump)
			printf("  Boot image entry:       0x%08x\n", ri.jump);
	} else {
		printf("  ROM log:                unavailable (%s)\n", why);
	}
	if (offs)
		printf("  Secondary image:        %s (IMG_CNTN_SET1_OFFSET=%u)\n",
		       offs, off);
	if (have_sec)
		printf("  Secure boot:            %s (SEC_CONFIG[1]=%u)\n",
		       sec ? "closed" : "open", sec);
	if (!strcmp(soc->soc_id, "i.MX7D") && bootinfo_7d())
		rc = EX_UNAVAIL;
	return rc;
}
