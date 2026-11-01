/* SPDX-License-Identifier: MIT */
/*
 * imx-sectool -- shared definitions.
 *
 * Author: Stefano Viola <stefano.viola@toradex.com>
 */
#ifndef SECTOOL_H
#define SECTOOL_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

/* mask of a w-bit field; 1u << 32 would be undefined */
#define FIELD_MASK(w) ((w) >= 32 ? 0xffffffffu : (1u << (w)) - 1)

#define VERSION "0.1.0"

/* exit codes (DESIGN.md, section 6.6); U-Boot clones use 0/1/2 only */
#define EX_OK 0
#define EX_FAIL 1
#define EX_USAGE 2
#define EX_UNAVAIL 3
#define EX_UNTRUSTED 4
#define EX_WARN 5

/* value a read-protected OCOTP word returns through the Linux driver */
#define OCOTP_READ_LOCKED 0xBADABADAu

/* ---- SoC model ---------------------------------------------------------- */

/* A physical window imx-sectool may map. Nothing outside these is touched. */
struct mem_region {
	const char *name;
	uint32_t base;
	uint32_t size;
};

/* A named bit-field in an OCOTP word (names as in the reference manual) */
struct fuse_field {
	const char *name;
	uint8_t bank, word, shift, width;
};

/* A whole OCOTP word, named as in the reference manual */
struct fuse_reg {
	const char *name;
	uint8_t bank, word;
};

struct soc_desc {
	const char *name; /* human name, e.g. "i.MX8M Plus" */
	const char *soc_id; /* /sys/devices/soc0/soc_id */
	unsigned int banks; /* OCOTP banks */
	unsigned int words_per_bank;
	const char *nvmem_prefix; /* nvmem device name prefix */

	/* SEC_CONFIG: HAB closed when (word & mask) == mask */
	unsigned int sec_bank, sec_word;
	uint32_t sec_mask;

	const struct mem_region *regions;
	size_t n_regions;

	int no_rom_log; /* the Boot ROM keeps no event log (i.MX7) */
	uint32_t rom_log_ptr; /* ROM address holding the ROM log pointer */
	uint32_t rom_log_lo; /* valid range for that pointer */
	uint32_t rom_log_hi;
	uint32_t hab_persist; /* HAB persistent area (EVT_DEF record) */
	uint32_t hab_persist_size; /* bytes searched for event records */
	uint32_t hab_evt_align; /* event record alignment, 8 if 0 */
	uint32_t src_base;
	uint32_t snvs_base;
	int srsr_reliable; /* SRSR still tells warm from cold in Linux */

	const struct fuse_field *fields; /* security-relevant fuses */
	size_t n_fields;
	unsigned int srk_bank, srk_words; /* SRK hash: srk_words from srk_bank */
	const char *srk_lock; /* name of the SRK hash lock field */
	const struct fuse_reg *regs;
	size_t n_regs;
	unsigned int mac_bank; /* MAC fuses: words 0-2 of this bank */
	int seco; /* SCU/SECO platform; only ahab_status is supported */
	uint32_t scu_mu; /* optional UIO mailbox base */
	int secure_jtag; /* JTAG_SMODE = Secure JTAG usable on this SoC */
};

extern const struct soc_desc *soc; /* detected SoC, set by soc_detect() */

int soc_detect(const char *force_id);

/* ---- global options ------------------------------------------------------ */

extern int opt_json;
extern const char *opt_nvmem; /* --nvmem: nvmem file override */

/* ---- backends ------------------------------------------------------------ */

/* class A: fuses through nvmem; index = bank * words_per_bank + word */
int nvmem_read(unsigned int index, uint32_t *val);
int nvmem_write(unsigned int index, uint32_t val);
const char *nvmem_path(void);

/* class B/C: whitelisted physical reads (or IMX_SECTOOL_MEM_DUMP files) */
int mmio_read32(uint32_t phys, uint32_t *val);
int mmio_read(uint32_t phys, void *buf, size_t len);
const char *mmio_error(void);

/* ---- fuse fields and shared readers -------------------------------------- */

#define FIELD_UNAVAIL (-1) /* nvmem read failed */
#define FIELD_LOCKED (-2) /* word is read-protected */

const struct fuse_field *field_find(const char *name);
const struct fuse_reg *reg_find(const char *name);
const char *field_meaning(const struct fuse_field *f, uint32_t v, char *buf,
			  size_t n);
int mac_read(int index, uint8_t mac[6]);
int field_read(const char *name, uint32_t *val);
int fuse_word(unsigned int bank, unsigned int word, uint32_t *val); /* same codes */

/* HAB event count from the ROM's persistent area; -1 with *why on failure */
int hab_event_count(const char **why);
const char *hab_code_name(char kind, uint8_t v); /* kind: 's', 'r', 'c' */

/* ROM log summary, shared by romlog and bootinfo */
struct romlog_info {
	uint32_t addr;
	int secondary; /* event 0x51 seen */
	int have_auth;
	uint32_t auth; /* event 0xa0 first parameter */
	int have_jump;
	uint32_t jump; /* event 0xc0 first parameter */
};
int romlog_summary(struct romlog_info *ri, const char **why);

/* ---- commands ------------------------------------------------------------ */

int cmd_fuse(int argc, char **argv);
int cmd_hab_status(int argc, char **argv);
int cmd_ahab_status(int argc, char **argv);
int cmd_romlog(int argc, char **argv);
int cmd_reset_cause(int argc, char **argv);
int cmd_debug_status(int argc, char **argv);
int cmd_audit(int argc, char **argv);
int cmd_fusemap(int argc, char **argv);
int cmd_bootinfo(int argc, char **argv);

#endif
