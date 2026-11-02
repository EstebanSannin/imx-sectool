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
	.hab_evt_align = 8,
	.src_base = 0x30390000,
	.src_sbmr1 = 0x58,
	.src_sbmr2 = 0x70,
	.src_srsr = 0x5c,
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
	.mac_count = 2,
	.srk_bank = 6,
	.srk_words = 8,
	.srk_lock = "CST_SRK_LOCK",
	.secure_jtag = 0, /* defeatured, NXP erratum ERR052318 */
};

/* ---- i.MX8M Mini ---------------------------------------------------------- */

/* Positions from the IMX8MM fusemap of usbarmory/crucible, the
 * meta-toradex-security imx8mm template (KTE, JTAG_HEO) and U-Boot
 * (SPEED_GRADING, TEMP_GRADE).
 */
static const struct fuse_field imx8mm_fields[] = {
	/* OCOTP_LOCK, bank 0 word 0 */
	{ "TESTER_LOCK", 0, 0, 0, 2 },
	{ "BOOT_CFG_LOCK", 0, 0, 2, 2 },
	{ "SRK_LOCK", 0, 0, 9, 1 },
	{ "SJC_RESP_LOCK", 0, 0, 10, 1 },
	{ "USB_ID_LOCK", 0, 0, 12, 2 },
	{ "MAC_ADDR_LOCK", 0, 0, 14, 2 },
	{ "MANUFACTURE_KEY_LOCK", 0, 0, 16, 1 },
	{ "GP1_LOCK", 0, 0, 20, 2 },
	{ "GP2_LOCK", 0, 0, 22, 2 },
	{ "GP5_LOCK", 0, 0, 24, 2 },
	/* OCOTP_TESTER3, bank 1 word 0 */
	{ "TEMP_GRADE", 1, 0, 6, 2 },
	{ "SPEED_GRADING", 1, 0, 8, 3 },
	/* OCOTP_TESTER4, bank 1 word 1 */
	{ "NUM_A53_CORES", 1, 1, 0, 2 },
	{ "M4_DISABLE", 1, 1, 8, 1 },
	{ "USB_OTG1_DISABLE", 1, 1, 11, 1 },
	{ "USB_OTG2_DISABLE", 1, 1, 12, 1 },
	{ "EXPORT_CONTROL", 1, 1, 16, 1 },
	{ "SEC_CONFIG[0]", 1, 1, 17, 1 },
	{ "VPU_G1_DISABLE", 1, 1, 18, 1 },
	{ "VPU_G2_DISABLE", 1, 1, 19, 1 },
	{ "VPU_H1_DISABLE", 1, 1, 20, 1 },
	{ "GPU2D_DISABLE", 1, 1, 21, 1 },
	{ "PCIE1_DISABLE", 1, 1, 22, 1 },
	{ "GPU3D_DISABLE", 1, 1, 24, 1 },
	{ "MIPI_DSI_DISABLE", 1, 1, 28, 1 },
	{ "ENET_DISABLE", 1, 1, 29, 1 },
	{ "MIPI_CSI_DISABLE", 1, 1, 30, 1 },
	/* OCOTP_BOOT_CFG0, bank 1 word 3 */
	{ "BOOT_CFG", 1, 3, 0, 16 },
	{ "KTE", 1, 3, 20, 1 },
	{ "SJC_DISABLE", 1, 3, 21, 1 },
	{ "JTAG_SMODE", 1, 3, 22, 2 }, /* 00 enabled, 01 secure, 11 no debug */
	{ "SEC_CONFIG[1]", 1, 3, 25, 1 }, /* closes the device */
	{ "JTAG_HEO", 1, 3, 26, 1 },
	{ "BT_FUSE_SEL", 1, 3, 28, 1 },
	{ "FORCE_COLD_BOOT", 1, 3, 29, 1 },
	/* OCOTP_BOOT_CFG1, bank 2 word 0 */
	{ "WDOG_ENABLE", 2, 0, 10, 1 },
	{ "TZASC_ENABLE", 2, 0, 11, 1 },
	{ "FORCE_INTERNAL_BOOT", 2, 0, 20, 1 },
	{ "SDP_DISABLE", 2, 0, 21, 1 },
	{ "SDP_READ_DISABLE", 2, 0, 22, 1 },
	/* bank 8 */
	{ "USB_VID", 8, 2, 0, 16 },
	{ "USB_PID", 8, 2, 16, 16 },
	{ "FIELD_RETURN", 8, 3, 0, 1 },
	/* bank 9 */
	{ "SRK_REVOKE", 9, 3, 0, 3 },
};

static const struct fuse_reg imx8mm_regs[] = {
	{ "OCOTP_LOCK", 0, 0 },	     { "OCOTP_TESTER0", 0, 1 },
	{ "OCOTP_TESTER1", 0, 2 },   { "OCOTP_TESTER3", 1, 0 },
	{ "OCOTP_TESTER4", 1, 1 },   { "OCOTP_TESTER5", 1, 2 },
	{ "OCOTP_BOOT_CFG0", 1, 3 }, { "OCOTP_BOOT_CFG1", 2, 0 },
	{ "OCOTP_BOOT_CFG2", 2, 1 }, { "OCOTP_BOOT_CFG3", 2, 2 },
	{ "OCOTP_BOOT_CFG4", 2, 3 }, { "OCOTP_TMU", 3, 3 },
	{ "OCOTP_SRK0", 6, 0 },	     { "OCOTP_SRK1", 6, 1 },
	{ "OCOTP_SRK2", 6, 2 },	     { "OCOTP_SRK3", 6, 3 },
	{ "OCOTP_SRK4", 7, 0 },	     { "OCOTP_SRK5", 7, 1 },
	{ "OCOTP_SRK6", 7, 2 },	     { "OCOTP_SRK7", 7, 3 },
	{ "OCOTP_SJC_RESP0", 8, 0 }, { "OCOTP_SJC_RESP1", 8, 1 },
	{ "OCOTP_USB_ID", 8, 2 },    { "OCOTP_FIELD_RETURN", 8, 3 },
	{ "OCOTP_MAC_ADDR0", 9, 0 }, { "OCOTP_MAC_ADDR1", 9, 1 },
	{ "OCOTP_MAC_ADDR2", 9, 2 }, { "OCOTP_SRK_REVOKE", 9, 3 },
	{ "OCOTP_GP10", 14, 0 },     { "OCOTP_GP11", 14, 1 },
	{ "OCOTP_GP20", 14, 2 },     { "OCOTP_GP21", 14, 3 },
};

/* No /dev/mem regions yet: the ROM, OCRAM and HAB addresses must first be
 * checked on hardware. TF-A sits at 0x920000 here, lower than on the 8MP.
 */
static const struct soc_desc imx8mm = {
	.name = "i.MX8M Mini",
	.soc_id = "i.MX8MM",
	.banks = 64,
	.words_per_bank = 4,
	.nvmem_prefix = "imx-ocotp",
	.sec_bank = 1,
	.sec_word = 3,
	.sec_mask = 0x02000000,
	.src_base = 0x30390000,
	.src_sbmr1 = 0x58,
	.src_sbmr2 = 0x70,
	.src_srsr = 0x5c,
	.snvs_base = 0x30370000,
	.fields = imx8mm_fields,
	.n_fields = ARRAY_SIZE(imx8mm_fields),
	.regs = imx8mm_regs,
	.n_regs = ARRAY_SIZE(imx8mm_regs),
	.mac_bank = 9,
	.mac_count = 2,
	.srk_bank = 6,
	.srk_words = 8,
	.srk_lock = "SRK_LOCK",
	.secure_jtag = 1,
};

/* ---- i.MX7Dual ------------------------------------------------------------ */

/* Read only the ROM software-info pointer. Broad ROM reads can hang i.MX7.
 */
static const struct mem_region imx7d_regions[] = {
	{ "ocram-hab", 0x00904000, 0x2000 },
	{ "rom-sw-info", 0x000001e8, 4 },
	{ "rom-log-ptr", 0x000001e0, 4 },
	{ "ocram-boot", 0x00900000, 0x20000 },
	{ "src", 0x30390000, 0x100 },
	{ "snvs-hp", 0x30370000, 0x100 },
};

/* Positions from the IMX7D fusemap of usbarmory/crucible, the
 * meta-toradex-security imx7 templates and U-Boot (SPEED_GRADING,
 * TEMP_GRADE).
 */
static const struct fuse_field imx7d_fields[] = {
	/* OCOTP_LOCK, bank 0 word 0 */
	{ "TESTER_LOCK", 0, 0, 0, 2 },
	{ "BOOT_CFG_LOCK", 0, 0, 2, 2 },
	{ "MEM_TRIM_LOCK", 0, 0, 4, 2 },
	{ "ANALOG_LOCK", 0, 0, 6, 2 },
	{ "OTPMK_LOCK", 0, 0, 8, 1 },
	{ "SRK_LOCK", 0, 0, 9, 1 },
	{ "SJC_RESP_LOCK", 0, 0, 10, 1 },
	{ "USB_ID_LOCK", 0, 0, 12, 2 },
	{ "MAC_ADDR_LOCK", 0, 0, 14, 2 },
	{ "MANUFACTURE_KEY_LOCK", 0, 0, 16, 1 },
	{ "ROM_PATCH_LOCK", 0, 0, 17, 1 },
	{ "GP1_LOCK", 0, 0, 20, 2 },
	{ "GP2_LOCK", 0, 0, 22, 2 },
	/* OCOTP_TESTER3, bank 1 word 0 */
	{ "TEMP_GRADE", 1, 0, 6, 2 },
	{ "SPEED_GRADING", 1, 0, 8, 2 },
	/* OCOTP_TESTER4, bank 1 word 1 */
	{ "NUM_A7_CORES", 1, 1, 0, 1 },
	{ "M4_DISABLE", 1, 1, 8, 1 },
	{ "SEC_CONFIG[0]", 1, 1, 17, 1 },
	{ "FLEXCAN_DISABLE", 1, 1, 21, 1 },
	{ "ADC_DISABLE", 1, 1, 22, 1 },
	{ "PCIE_DISABLE", 1, 1, 23, 1 },
	{ "EPDC_DISABLE", 1, 1, 28, 1 },
	{ "ENET1_DISABLE", 1, 1, 29, 1 },
	{ "ENET2_DISABLE", 1, 1, 30, 1 },
	{ "MIPI_DISABLE", 1, 1, 31, 1 },
	/* OCOTP_BOOT_CFG0, bank 1 word 3 */
	{ "BOOT_CFG", 1, 3, 0, 16 },
	{ "KTE", 1, 3, 20, 1 },
	{ "SJC_DISABLE", 1, 3, 21, 1 },
	{ "JTAG_SMODE", 1, 3, 22, 2 }, /* 00 enabled, 01 secure, 11 no debug */
	{ "SEC_CONFIG[1]", 1, 3, 25, 1 }, /* closes the device */
	{ "JTAG_HEO", 1, 3, 26, 1 },
	{ "DIR_BT_DIS", 1, 3, 27, 1 },
	{ "BT_FUSE_SEL", 1, 3, 28, 1 },
	{ "FORCE_COLD_BOOT", 1, 3, 29, 1 },
	/* OCOTP_BOOT_CFG1, bank 2 word 0 */
	{ "SDP_READ_DISABLE", 2, 0, 8, 1 },
	{ "SDP_DISABLE", 2, 0, 9, 1 },
	{ "WDOG_ENABLE", 2, 0, 10, 1 },
	{ "TZASC_ENABLE", 2, 0, 11, 1 },
	/* bank 8 */
	{ "USB_VID", 8, 2, 0, 16 },
	{ "USB_PID", 8, 2, 16, 16 },
	{ "FIELD_RETURN", 8, 3, 0, 1 },
	/* bank 9 */
	{ "SRK_REVOKE", 9, 3, 0, 3 },
};

static const struct fuse_reg imx7d_regs[] = {
	{ "OCOTP_LOCK", 0, 0 },	      { "OCOTP_TESTER0", 0, 1 },
	{ "OCOTP_TESTER1", 0, 2 },    { "OCOTP_TESTER2", 0, 3 },
	{ "OCOTP_TESTER3", 1, 0 },    { "OCOTP_TESTER4", 1, 1 },
	{ "OCOTP_TESTER5", 1, 2 },    { "OCOTP_BOOT_CFG0", 1, 3 },
	{ "OCOTP_BOOT_CFG1", 2, 0 },  { "OCOTP_BOOT_CFG2", 2, 1 },
	{ "OCOTP_BOOT_CFG3", 2, 2 },  { "OCOTP_BOOT_CFG4", 2, 3 },
	{ "OCOTP_MEM_TRIM0", 3, 0 },  { "OCOTP_MEM_TRIM1", 3, 1 },
	{ "OCOTP_MEM_ANA0", 3, 2 },   { "OCOTP_MEM_ANA1", 3, 3 },
	{ "OCOTP_SRK0", 6, 0 },	      { "OCOTP_SRK1", 6, 1 },
	{ "OCOTP_SRK2", 6, 2 },	      { "OCOTP_SRK3", 6, 3 },
	{ "OCOTP_SRK4", 7, 0 },	      { "OCOTP_SRK5", 7, 1 },
	{ "OCOTP_SRK6", 7, 2 },	      { "OCOTP_SRK7", 7, 3 },
	{ "OCOTP_SJC_RESP0", 8, 0 },  { "OCOTP_SJC_RESP1", 8, 1 },
	{ "OCOTP_USB_ID", 8, 2 },     { "OCOTP_FIELD_RETURN", 8, 3 },
	{ "OCOTP_MAC_ADDR0", 9, 0 },  { "OCOTP_MAC_ADDR1", 9, 1 },
	{ "OCOTP_MAC_ADDR2", 9, 2 },  { "OCOTP_SRK_REVOKE", 9, 3 },
	{ "OCOTP_GP10", 14, 0 },      { "OCOTP_GP11", 14, 1 },
	{ "OCOTP_GP20", 14, 2 },      { "OCOTP_GP21", 14, 3 },
};

/* HAB store 0 comes from the store table at ROM 0x11598; its records are
 * packed on 4-byte boundaries.
 */
static const struct soc_desc imx7d = {
	.name = "i.MX7Dual",
	.soc_id = "i.MX7D",
	.banks = 16,
	.words_per_bank = 4,
	.nvmem_prefix = "imx-ocotp",
	.sec_bank = 1,
	.sec_word = 3,
	.sec_mask = 0x02000000,
	.regions = imx7d_regions,
	.n_regions = ARRAY_SIZE(imx7d_regions),
	.rom_log_words = 64,
	.rom_log_ptr = 0x1e0,
	.rom_log_lo = 0x00900000,
	.rom_log_hi = 0x00920000,
	.hab_persist = 0x009049c0,
	.hab_persist_size = 0xb80,
	.hab_evt_align = 4,
	.src_base = 0x30390000,
	.src_sbmr1 = 0x58,
	.src_sbmr2 = 0x70,
	.src_srsr = 0x5c,
	.snvs_base = 0x30370000,
	.srsr_reliable = 0, /* U-Boot clears SRSR after reading it */
	.fields = imx7d_fields,
	.n_fields = ARRAY_SIZE(imx7d_fields),
	.regs = imx7d_regs,
	.n_regs = ARRAY_SIZE(imx7d_regs),
	.mac_bank = 9,
	.mac_count = 2,
	.srk_bank = 6,
	.srk_words = 8,
	.srk_lock = "SRK_LOCK",
	.secure_jtag = 1,
};

/* ---- i.MX6DualLite -------------------------------------------------------- */

/* Fields from usbarmory/crucible IMX6DL (BSD-3-Clause, crucible authors).
 * Speed grade: U-Boot mx6/soc.c. SRK revocation: NXP AN4581, appendix B.
 */
static const struct fuse_field imx6dl_fields[] = {
	{ "TESTER_LOCK", 0, 0, 0, 2 },
	{ "BOOT_CFG_LOCK", 0, 0, 2, 2 },
	{ "MEM_TRIM_LOCK", 0, 0, 4, 2 },
	{ "SJC_RESP_LOCK", 0, 0, 6, 1 },
	{ "MAC_ADDR_LOCK", 0, 0, 8, 2 },
	{ "GP1_LOCK", 0, 0, 10, 2 },
	{ "GP2_LOCK", 0, 0, 12, 2 },
	{ "SRK_LOCK", 0, 0, 14, 1 },
	{ "ANALOG_LOCK", 0, 0, 18, 2 },
	{ "MISC_CONF_LOCK", 0, 0, 22, 1 },
	{ "SI_REV", 0, 3, 16, 4 },
	{ "NUM_CORES", 0, 3, 20, 2 },
	{ "BOOT_CFG1", 0, 5, 0, 8 },
	{ "BOOT_CFG2", 0, 5, 8, 8 },
	{ "BOOT_CFG3", 0, 5, 16, 8 },
	{ "BOOT_CFG4", 0, 5, 24, 8 },
	{ "SEC_CONFIG", 0, 6, 1, 1 },
	{ "DIR_BT_DIS", 0, 6, 3, 1 },
	{ "BT_FUSE_SEL", 0, 6, 4, 1 },
	{ "SJC_DISABLE", 0, 6, 20, 1 },
	{ "WDOG_ENABLE", 0, 6, 21, 1 },
	{ "JTAG_SMODE", 0, 6, 22, 2 },
	{ "KTE", 0, 6, 26, 1 },
	{ "JTAG_HEO", 0, 6, 27, 1 },
	{ "TZASC_ENABLE", 0, 6, 28, 1 },
	{ "SDMMC_HYS_EN", 0, 6, 29, 1 },
	{ "EMMC_RESET_EN", 0, 6, 30, 1 },
	{ "NAND_READ_CMD_CODE1", 0, 7, 0, 8 },
	{ "NAND_READ_CMD_CODE2", 0, 7, 8, 8 },
	{ "BT_LPB_POLARITY", 0, 7, 20, 1 },
	{ "LPB_BOOT", 0, 7, 21, 2 },
	{ "MMC_DLL_DLY", 0, 7, 24, 7 },
	{ "TEMP_GRADE", 1, 0, 6, 2 },
	{ "USB_VID", 1, 7, 0, 16 },
	{ "USB_PID", 1, 7, 16, 16 },
	{ "GP1", 4, 6, 0, 32 },
	{ "GP2", 4, 7, 0, 32 },
	{ "PAD_SETTINGS", 5, 5, 0, 6 },
	{ "FIELD_RETURN", 5, 6, 0, 1 },
	{ "SPEED_GRADING", 0, 4, 16, 2 },
	{ "SRK_REVOKE", 5, 7, 0, 3 },
};

static const struct fuse_reg imx6dl_regs[] = {
	{ "OCOTP_LOCK", 0, 0 },
	{ "OCOTP_CFG0", 0, 1 },
	{ "OCOTP_CFG1", 0, 2 },
	{ "OCOTP_CFG2", 0, 3 },
	{ "OCOTP_CFG3", 0, 4 },
	{ "OCOTP_CFG4", 0, 5 },
	{ "OCOTP_CFG5", 0, 6 },
	{ "OCOTP_CFG6", 0, 7 },
	{ "OCOTP_MEM0", 1, 0 },
	{ "OCOTP_MEM1", 1, 1 },
	{ "OCOTP_MEM2", 1, 2 },
	{ "OCOTP_MEM3", 1, 3 },
	{ "OCOTP_MEM4", 1, 4 },
	{ "OCOTP_ANA0", 1, 5 },
	{ "OCOTP_ANA1", 1, 6 },
	{ "OCOTP_ANA2", 1, 7 },
	{ "OCOTP_SRK0", 3, 0 },
	{ "OCOTP_SRK1", 3, 1 },
	{ "OCOTP_SRK2", 3, 2 },
	{ "OCOTP_SRK3", 3, 3 },
	{ "OCOTP_SRK4", 3, 4 },
	{ "OCOTP_SRK5", 3, 5 },
	{ "OCOTP_SRK6", 3, 6 },
	{ "OCOTP_SRK7", 3, 7 },
	{ "OCOTP_SJC_RESP0", 4, 0 },
	{ "OCOTP_SJC_RESP1", 4, 1 },
	{ "OCOTP_MAC0", 4, 2 },
	{ "OCOTP_MAC1", 4, 3 },
	{ "OCOTP_GP1", 4, 6 },
	{ "OCOTP_GP2", 4, 7 },
	{ "OCOTP_MISC_CONF", 5, 5 },
	{ "OCOTP_FIELD_RETURN", 5, 6 },
	{ "OCOTP_SRK_REVOKE", 5, 7 },
};

static const struct mem_region imx6dl_regions[] = {
	{ "rom-rvt", 0x00000094, 0x48 },
	{ "ocram-log", 0x00900000, 0x4000 },
	{ "ocram-hab", 0x00904000, 0xb80 },
	{ "src", 0x020d8000, 0x48 },
	{ "snvs-hp", 0x020cc014, 4 },
};

static const struct soc_desc imx6dl = {
	.name = "i.MX6DualLite",
	.soc_id = "i.MX6DL",
	.banks = 16,
	.words_per_bank = 8,
	.nvmem_prefix = "imx-ocotp",
	.sec_bank = 0,
	.sec_word = 6,
	.sec_mask = 2,
	.regions = imx6dl_regions,
	.n_regions = ARRAY_SIZE(imx6dl_regions),
	.rom_log_words = 128,
	.rom_log_ptr = 0xd8,
	.rom_log_lo = 0x00900000,
	.rom_log_hi = 0x00904000,
	.hab_persist = 0x00904000,
	.hab_persist_size = 0xb80,
	.hab_evt_align = 4,
	.src_base = 0x020d8000,
	.snvs_base = 0x020cc000,
	.src_sbmr1 = 0x04,
	.src_sbmr2 = 0x1c,
	.src_srsr = 0x08,
	.fields = imx6dl_fields,
	.n_fields = ARRAY_SIZE(imx6dl_fields),
	.regs = imx6dl_regs,
	.n_regs = ARRAY_SIZE(imx6dl_regs),
	.mac_bank = 4,
	.mac_word = 2,
	.mac_count = 1,
	.srk_bank = 3,
	.srk_words = 8,
	.srk_lock = "SRK_LOCK",
	.secure_jtag = 1,
};

static const struct soc_desc imx8qm = {
	.name = "i.MX8QuadMax",
	.soc_id = "i.MX8QM",
	.seco = 1,
	.scu_mu = 0x5d1d0000,
};

static const struct soc_desc imx8qxp = {
	.name = "i.MX8QuadXPlus",
	.soc_id = "i.MX8QXP",
	.seco = 1,
	.scu_mu = 0x5d1d0000,
};

static const struct soc_desc *const socs[] = {
	&imx8mp, &imx8mm, &imx7d, &imx6dl, &imx8qm, &imx8qxp,
};

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

const char *secure_boot_field(void)
{
	return field_find("SEC_CONFIG") ? "SEC_CONFIG" : "SEC_CONFIG[1]";
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
		"error: SoC '%s' is not supported yet (supported: i.MX6DL, i.MX8MP, i.MX8MM, i.MX7D, i.MX8QM, i.MX8QXP)\n",
		id);
	return EX_UNAVAIL;
}
