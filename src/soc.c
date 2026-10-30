// SPDX-License-Identifier: MIT
/*
 * imx-sectool -- SoC detection and descriptors.
 *
 * Every address and fuse position the tool uses lives here, per SoC. Values
 * come from U-Boot / TF-A sources and were verified on hardware; see
 * DESIGN.md section 8 and docs/phase0-imx8mp.md.
 *
 * Author: Stefano Viola <stefano.viola@toradex.com>
 */
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <errno.h>

#include "sectool.h"

#define SOC_ID_PATH "/sys/devices/soc0/soc_id"

/* ---- i.MX8M Plus ---------------------------------------------------------- */

static const struct mem_region imx8mp_regions[] = {
	{ "rom-low", 0x00000000, 0x1000 }, /* HAB RVT, ROM API, ROM log ptr */
	{ "ocram-low", 0x00900000,
	  0x40000 }, /* below TF-A (NXP BL31 at 0x950000) */
	{ "src", 0x30390000, 0x100 },
	{ "snvs-hp", 0x30370000, 0x100 },
};

/* Positions from the IMX8MP fusemap of usbarmory/crucible (BSD-3-Clause,
 * Copyright (c) The crucible authors), the meta-toradex-security imx8mp
 * templates and U-Boot (TEMP_GRADE: get_cpu_temp_grade(),
 * IMG_CNTN_SET1_OFFSET: arch_spl_mmc_get_uboot_raw_sector()).
 */
static const struct fuse_field imx8mp_fields[] = {
	/* OCOTP_LOCK, bank 0 word 0 -- 2-bit locks: bit 0 write-protect, bit 1 override-protect */
	{ "TESTER_LOCK", 0, 0, 0, 2 },
	{ "BOOT_CFG_LOCK", 0, 0, 2, 2 },
	{ "CST_SRK_LOCK", 0, 0, 9, 1 },
	{ "SJC_RESP_LOCK", 0, 0, 10, 1 },
	{ "USB_ID_LOCK", 0, 0, 12, 2 },
	{ "MAC_ADDR_LOCK", 0, 0, 14, 2 },
	{ "GP1_LOCK", 0, 0, 20, 2 },
	{ "GP2_LOCK", 0, 0, 22, 2 },
	/* OCOTP_TESTER3, bank 1 word 0 */
	{ "TEMP_GRADE", 1, 0, 5, 2 },
	{ "SPEED_GRADING", 1, 0, 8, 6 },
	{ "EXPORT_CONTROL", 1, 0, 16, 1 },
	{ "SEC_CONFIG[0]", 1, 0, 17, 1 },
	{ "M7_DISABLE", 1, 0, 21, 1 },
	{ "VPU_G1_DISABLE", 1, 0, 24, 1 },
	{ "VPU_G2_DISABLE", 1, 0, 25, 1 },
	{ "CAN_DISABLE", 1, 0, 28, 1 },
	{ "CAN_FD_DISABLE", 1, 0, 29, 1 },
	{ "VPU_VC8000E_DISABLE", 1, 0, 30, 1 },
	/* OCOTP_TESTER4, bank 1 word 1 */
	{ "IMG_ISP1_DISABLE", 1, 1, 0, 1 },
	{ "IMG_ISP2_DISABLE", 1, 1, 1, 1 },
	{ "IMG_DEWARP_DISABLE", 1, 1, 2, 1 },
	{ "NPU_DISABLE", 1, 1, 3, 1 },
	{ "AUDIO_PROCESSOR_DISABLE", 1, 1, 4, 1 },
	{ "GPU2D_DISABLE", 1, 1, 6, 1 },
	{ "GPU3D_DISABLE", 1, 1, 7, 1 },
	{ "PCIE1_DISABLE", 1, 1, 11, 1 },
	{ "ENET1_DISABLE", 1, 1, 13, 1 },
	{ "ENET2_DISABLE", 1, 1, 14, 1 },
	/* OCOTP_BOOT_CFG0, bank 1 word 3 */
	{ "BOOT_CFG", 1, 3, 0, 16 },
	{ "BOOT_MODE_FUSES", 1, 3, 12, 4 },
	{ "KTE", 1, 3, 20, 1 },
	{ "SJC_DISABLE", 1, 3, 21, 1 },
	{ "JTAG_SMODE", 1, 3, 22, 2 }, /* 00 enabled, 01 secure, 11 no debug */
	{ "SEC_CONFIG[1]", 1, 3, 25, 1 }, /* closes the device */
	{ "JTAG_HEO", 1, 3, 26, 1 },
	{ "BT_FUSE_SEL", 1, 3, 28, 1 },
	{ "FORCE_COLD_BOOT", 1, 3, 29, 1 },
	/* OCOTP_BOOT_CFG1..2, bank 2 words 0-1 */
	{ "WDOG_EN", 2, 0, 10, 1 },
	{ "TZASC_ENABLE", 2, 0, 11, 1 },
	{ "FORCE_BT_FROM_FUSE", 2, 0, 20, 1 },
	{ "SDP_DISABLE", 2, 0, 21, 1 },
	{ "ROM_NO_LOG", 2, 0, 22, 1 },
	{ "IMG_CNTN_SET1_OFFSET", 2, 1, 19, 4 },
	{ "RECOVERY_SDMMC_BOOT_DIS", 2, 1, 23, 1 },
	/* bank 8 */
	{ "USB_VID", 8, 2, 0, 16 },
	{ "USB_PID", 8, 2, 16, 16 },
	{ "FIELD_RETURN", 8, 3, 0, 1 },
	/* bank 9 */
	{ "SRK_REVOKE", 9, 3, 0, 4 },
};

static const struct fuse_reg imx8mp_regs[] = {
	{ "OCOTP_LOCK", 0, 0 },	     { "OCOTP_TESTER0", 0, 1 },
	{ "OCOTP_TESTER1", 0, 2 },   { "OCOTP_TESTER2", 0, 3 },
	{ "OCOTP_TESTER3", 1, 0 },   { "OCOTP_TESTER4", 1, 1 },
	{ "OCOTP_TESTER5", 1, 2 },   { "OCOTP_BOOT_CFG0", 1, 3 },
	{ "OCOTP_BOOT_CFG1", 2, 0 }, { "OCOTP_BOOT_CFG2", 2, 1 },
	{ "OCOTP_BOOT_CFG3", 2, 2 }, { "OCOTP_BOOT_CFG4", 2, 3 },
	{ "OCOTP_SRK0", 6, 0 },	     { "OCOTP_SRK1", 6, 1 },
	{ "OCOTP_SRK2", 6, 2 },	     { "OCOTP_SRK3", 6, 3 },
	{ "OCOTP_SRK4", 7, 0 },	     { "OCOTP_SRK5", 7, 1 },
	{ "OCOTP_SRK6", 7, 2 },	     { "OCOTP_SRK7", 7, 3 },
	{ "OCOTP_SJC_RESP0", 8, 0 }, { "OCOTP_SJC_RESP1", 8, 1 },
	{ "OCOTP_USB_ID", 8, 2 },    { "OCOTP_FIELD_RETURN", 8, 3 },
	{ "OCOTP_MAC_ADDR0", 9, 0 }, { "OCOTP_MAC_ADDR1", 9, 1 },
	{ "OCOTP_MAC_ADDR2", 9, 2 }, { "OCOTP_SRK_REVOKE", 9, 3 },
};

static const struct soc_desc imx8mp = {
	.name = "i.MX8M Plus",
	.soc_id = "i.MX8MP",
	.banks = 96,
	.words_per_bank = 4,
	.nvmem_prefix = "imx-ocotp",
	.sec_bank = 1,
	.sec_word = 3,
	.sec_mask = 0x02000000,
	.regions = imx8mp_regions,
	.n_regions = ARRAY_SIZE(imx8mp_regions),
	.rom_log_ptr = 0x9e0,
	.rom_log_lo = 0x00900000,
	.rom_log_hi = 0x00b00000,
	.hab_persist = 0x0090d040,
	.hab_persist_size = 0x400,
	.src_base = 0x30390000,
	.snvs_base = 0x30370000,
	/* power-cycle, reboot, sysrq reboot and panic restart all leave
	 * SRSR = 0x1 by the time Linux runs (docs/phase0-imx8mp.md)
	 */
	.srsr_reliable = 0,
	.fields = imx8mp_fields,
	.n_fields = ARRAY_SIZE(imx8mp_fields),
	.regs = imx8mp_regs,
	.n_regs = ARRAY_SIZE(imx8mp_regs),
	.mac_bank = 9,
	.srk_bank = 6,
	.srk_words = 8,
	.secure_jtag = 0, /* defeatured, NXP erratum ERR052318 */
};

static const struct soc_desc *const socs[] = { &imx8mp };

const struct soc_desc *soc;

/* ---- fuse fields --------------------------------------------------------------- */

/* Names match case-insensitively and ignoring '[' and ']', so the shell-
 * friendly SEC_CONFIG1 finds SEC_CONFIG[1].
 */
static int name_eq(const char *a, const char *b)
{
	for (;;) {
		while (*a == '[' || *a == ']')
			a++;
		while (*b == '[' || *b == ']')
			b++;
		if (!*a || !*b)
			return *a == *b;
		if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
			return 0;
		a++;
		b++;
	}
}

const struct fuse_field *field_find(const char *name)
{
	size_t i;

	for (i = 0; i < soc->n_fields; i++)
		if (name_eq(soc->fields[i].name, name))
			return &soc->fields[i];
	return NULL;
}

const struct fuse_reg *reg_find(const char *name)
{
	size_t i;

	for (i = 0; i < soc->n_regs; i++)
		if (!strcasecmp(soc->regs[i].name, name))
			return &soc->regs[i];
	return NULL;
}

int fuse_word(unsigned int bank, unsigned int word, uint32_t *val)
{
	if (nvmem_read(bank * soc->words_per_bank + word, val))
		return FIELD_UNAVAIL;
	if (*val == OCOTP_READ_LOCKED)
		return FIELD_LOCKED;
	return 0;
}

int field_read(const char *name, uint32_t *val)
{
	const struct fuse_field *f = field_find(name);
	uint32_t w;
	int rc;

	if (!f)
		return FIELD_UNAVAIL;
	rc = fuse_word(f->bank, f->word, &w);
	if (rc)
		return rc;
	*val = (w >> f->shift) & FIELD_MASK(f->width);
	return 0;
}

/* Select the descriptor: --soc wins, else /sys/devices/soc0/soc_id. */
int soc_detect(const char *force_id)
{
	char buf[64] = { 0 };
	const char *id = force_id;
	size_t i;
	FILE *f;

	if (!id) {
		f = fopen(SOC_ID_PATH, "r");
		if (!f) {
			fprintf(stderr,
				"error: cannot read %s (%s)\n"
				"hint:  pass --soc <id>, e.g. --soc i.MX8MP\n",
				SOC_ID_PATH, strerror(errno));
			return EX_UNAVAIL;
		}
		if (!fgets(buf, sizeof(buf), f))
			buf[0] = '\0';
		fclose(f);
		buf[strcspn(buf, "\n")] = '\0';
		id = buf;
	}
	for (i = 0; i < ARRAY_SIZE(socs); i++) {
		if (!strcmp(socs[i]->soc_id, id)) {
			soc = socs[i];
			return EX_OK;
		}
	}
	fprintf(stderr,
		"error: SoC '%s' is not supported yet (supported: i.MX8MP)\n",
		id);
	return EX_UNAVAIL;
}
