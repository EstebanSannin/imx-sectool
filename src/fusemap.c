// SPDX-License-Identifier: MIT
/*
 * imx-sectool -- 'fusemap': named fuses, decoded.
 *
 *   fusemap                 every named field with its value and meaning
 *   fusemap <FIELD>         one field: position, value, meaning, its word
 *   fusemap <REGISTER>      one OCOTP word (e.g. OCOTP_BOOT_CFG0) and the
 *                           fields inside it
 *   fusemap SRK_HASH | MAC0 | MAC1
 *                           composite values spread over several words
 *
 * Meanings are only given where a source backs them: U-Boot for the speed
 * and temperature grades, the MAC byte order and IMG_CNTN_SET1_OFFSET; the
 * reference manual field descriptions (via crucible and meta-toradex-security)
 * for locks and JTAG_SMODE.
 *
 * Author: Stefano Viola <stefano.viola@toradex.com>
 */
#define _POSIX_C_SOURCE 200809L
#include <string.h>
#include <strings.h>

#include "sectool.h"

/* ---- decoding ------------------------------------------------------------------ */

static int ends_with(const char *s, const char *suffix)
{
	size_t a = strlen(s), b = strlen(suffix);

	return a >= b && !strcmp(s + a - b, suffix);
}

/* Human meaning of a field value, or NULL when there is nothing to add. */
const char *field_meaning(const struct fuse_field *f, uint32_t v, char *buf,
			  size_t n)
{
	static const char *const lock2[] = { "unlocked", "write-protect",
					     "override-protect",
					     "write+override-protect" };
	static const char *const temp[] = { "commercial (0..95 C)",
					    "extended commercial (-20..105 C)",
					    "industrial (-40..105 C)",
					    "automotive (-40..125 C)" };
	static const unsigned int speed_8mm[] = { 800,	1200, 1600, 1800,
						  2000, 0,    0,    0 };
	static const unsigned int speed_7d[] = { 800, 500, 1000, 1200 };
	unsigned int mib;
	char *p;
	int k;

	if (ends_with(f->name, "_LOCK") && f->width == 2)
		return lock2[v & 3];
	if (ends_with(f->name, "_LOCK") && f->width == 1)
		return v ? "locked" : "unlocked";
	if (!strcmp(f->name, "JTAG_SMODE"))
		return v == 0 ? "JTAG enabled" :
		       v == 1 ? "Secure JTAG" :
		       v == 3 ? "No debug" :
				"reserved";
	if (!strcmp(f->name, "SEC_CONFIG[1]"))
		return v ? "closed" : "open";
	if (!strcmp(f->name, "TEMP_GRADE"))
		return temp[v & 3];
	/* U-Boot get_cpu_speed_grade_hz() */
	if (!strcmp(f->name, "SPEED_GRADING")) {
		if (!strcmp(soc->soc_id, "i.MX8MM"))
			snprintf(buf, n, "%u MHz", speed_8mm[v & 7]);
		else if (!strcmp(soc->soc_id, "i.MX7D"))
			snprintf(buf, n, "%u MHz", speed_7d[v & 3]);
		else
			snprintf(buf, n, "%u MHz", 2300 - (v & 0xf) * 100);
		return buf;
	}
	/* U-Boot arch_spl_mmc_get_uboot_raw_sector() */
	if (!strcmp(f->name, "IMG_CNTN_SET1_OFFSET")) {
		if (v > 10)
			return "secondary image boot disabled";
		mib = v == 0 ? 4 : v == 1 ? 2 : v == 2 ? 1 : 1u << v;
		snprintf(buf, n, "secondary image at %u MiB", mib);
		return buf;
	}
	if (!strcmp(f->name, "SRK_REVOKE")) {
		if (!v)
			return "no key revoked";
		p = buf;
		p += snprintf(p, n, "revoked:");
		for (k = 0; k < f->width; k++)
			if (v & (1u << k))
				p += snprintf(p, n - (size_t)(p - buf),
					      " SRK%d", k + 1);
		return buf;
	}
	if (!strcmp(f->name, "BT_FUSE_SEL"))
		return v ? "boot config from fuses" : "boot config from pins";
	return NULL;
}

/*
 * MAC address as U-Boot's imx_get_mac_from_fuse() assembles it. Returns -1 if
 * the fuses cannot be read. index 0 = ENET/EQOS 0, 1 = second MAC.
 */
int mac_read(int index, uint8_t mac[6])
{
	uint32_t w0, w1, w2;

	if (fuse_word(soc->mac_bank, 0, &w0) ||
	    fuse_word(soc->mac_bank, 1, &w1) ||
	    fuse_word(soc->mac_bank, 2, &w2))
		return -1;
	if (index == 1) {
		mac[0] = w2 >> 24;
		mac[1] = w2 >> 16;
		mac[2] = w2 >> 8;
		mac[3] = w2;
		mac[4] = w1 >> 24;
		mac[5] = w1 >> 16;
	} else {
		mac[0] = w1 >> 8;
		mac[1] = w1;
		mac[2] = w0 >> 24;
		mac[3] = w0 >> 16;
		mac[4] = w0 >> 8;
		mac[5] = w0;
	}
	return 0;
}

/* ---- output ------------------------------------------------------------------- */

static void bits(const struct fuse_field *f, char *buf, size_t n)
{
	if (f->width == 1)
		snprintf(buf, n, "%u", f->shift);
	else
		snprintf(buf, n, "%u:%u", f->shift + f->width - 1, f->shift);
}

static int first_json;

static void field_line(const struct fuse_field *f)
{
	char b[16], mb[64];
	const char *m;
	uint32_t v;
	int rc;

	bits(f, b, sizeof(b));
	rc = field_read(f->name, &v);
	m = rc ? NULL : field_meaning(f, v, mb, sizeof(mb));
	if (opt_json) {
		printf("%s\n  {\"name\": \"%s\", \"bank\": %u, \"word\": %u, \"bits\": \"%s\", ",
		       first_json ? "" : ",", f->name, f->bank, f->word, b);
		first_json = 0;
		if (rc)
			printf("\"value\": null, \"status\": \"%s\"}",
			       rc == FIELD_LOCKED ? "read-protected" :
						    "unreadable");
		else {
			printf("\"value\": %u", v);
			if (m)
				printf(", \"meaning\": \"%s\"", m);
			printf("}");
		}
		return;
	}
	printf("%-24s %2u/%u  %-5s  ", f->name, f->bank, f->word, b);
	if (rc)
		printf("%s\n",
		       rc == FIELD_LOCKED ? "read-protected" : "unreadable");
	else if (m)
		printf("0x%-6x %s\n", v, m);
	else
		printf("0x%x\n", v);
}

static int show_mac(int index)
{
	uint8_t m[6];
	int empty;

	if (mac_read(index, m)) {
		fprintf(stderr, "error: MAC fuses unreadable\n");
		return EX_UNAVAIL;
	}
	empty = !(m[0] | m[1] | m[2] | m[3] | m[4] | m[5]);
	if (opt_json)
		printf("{\"name\": \"MAC%d\", \"bank\": %u, \"value\": \"%02x:%02x:%02x:%02x:%02x:%02x\", \"programmed\": %s}\n",
		       index, soc->mac_bank, m[0], m[1], m[2], m[3], m[4], m[5],
		       empty ? "false" : "true");
	else
		printf("MAC%d (bank %u words %s): %02x:%02x:%02x:%02x:%02x:%02x%s\n",
		       index, soc->mac_bank, index ? "1-2" : "0-1", m[0], m[1],
		       m[2], m[3], m[4], m[5],
		       empty ? "  (not programmed)" : "");
	return EX_OK;
}

static int show_srk(void)
{
	int locked = 0, unread = 0, any = 0, rc, lrc;
	unsigned int i, idx;
	uint32_t w[16], lk = 0;

	for (i = 0; i < soc->srk_words && i < 16; i++) {
		idx = soc->srk_bank * soc->words_per_bank + i;
		rc = fuse_word(idx / soc->words_per_bank,
			       idx % soc->words_per_bank, &w[i]);
		if (rc == FIELD_LOCKED)
			locked = 1;
		else if (rc)
			unread = 1;
		else if (w[i])
			any = 1;
	}
	if (unread) {
		fprintf(stderr, "error: SRK fuses unreadable\n");
		return EX_UNAVAIL;
	}
	lrc = field_read(soc->srk_lock, &lk);
	if (opt_json) {
		printf("{\"name\": \"SRK_HASH\", \"bank\": %u, \"words\": [",
		       soc->srk_bank);
		for (i = 0; i < soc->srk_words; i++)
			printf("%s\"0x%08x\"", i ? ", " : "", w[i]);
		printf("], \"programmed\": %s, \"locked\": %s}\n",
		       any || locked ? "true" : "false",
		       lrc ? "null" :
		       lk  ? "true" :
			     "false");
		return EX_OK;
	}
	printf("SRK_HASH (bank %u, %u words):", soc->srk_bank, soc->srk_words);
	for (i = 0; i < soc->srk_words; i++)
		printf(" %08x", w[i]);
	printf("\n  %s, %s=%s\n",
	       any || locked ? "programmed" : "not programmed", soc->srk_lock,
	       lrc ? "?" :
	       lk  ? "1 (locked)" :
		     "0 (unlocked)");
	return EX_OK;
}

static int show_reg(const struct fuse_reg *r)
{
	const struct fuse_field *f;
	uint32_t w;
	size_t i;
	int rc;

	rc = fuse_word(r->bank, r->word, &w);
	if (rc == FIELD_UNAVAIL)
		return EX_UNAVAIL;
	if (opt_json) {
		printf("{\"name\": \"%s\", \"bank\": %u, \"word\": %u, ",
		       r->name, r->bank, r->word);
		if (rc)
			printf("\"value\": null, \"status\": \"read-protected\", \"fields\": [");
		else
			printf("\"value\": \"0x%08x\", \"fields\": [", w);
		first_json = 1;
	} else {
		printf("%s (bank %u word %u): ", r->name, r->bank, r->word);
		if (rc)
			printf("read-protected\n");
		else
			printf("0x%08x\n", w);
	}
	for (i = 0; i < soc->n_fields; i++) {
		f = &soc->fields[i];
		if (f->bank != r->bank || f->word != r->word)
			continue;
		if (!opt_json)
			printf("  ");
		field_line(f);
	}
	if (opt_json)
		printf("\n]}\n");
	return EX_OK;
}

int cmd_fusemap(int argc, char **argv)
{
	const struct fuse_field *f;
	const struct fuse_reg *r;
	const char *name = NULL;
	char b[16], mb[64];
	const char *m;
	uint32_t w, v;
	size_t j;
	int i, rc;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--json"))
			continue;
		if (name) {
			fprintf(stderr,
				"usage: imx-sectool fusemap [<FIELD|REGISTER|SRK_HASH|MAC0|MAC1>]\n");
			return EX_USAGE;
		}
		name = argv[i];
	}

	if (!name) {
		if (opt_json) {
			printf("{\"soc\": \"%s\", \"fields\": [", soc->soc_id);
			first_json = 1;
		} else {
			printf("%-24s %-5s  %-5s  %s\n", "FIELD", "BK/WD",
			       "BITS", "VALUE");
		}
		for (j = 0; j < soc->n_fields; j++)
			field_line(&soc->fields[j]);
		if (opt_json)
			printf("\n]}\n");
		return EX_OK;
	}

	if (!strcasecmp(name, "SRK_HASH"))
		return show_srk();
	if (!strcasecmp(name, "MAC0"))
		return show_mac(0);
	if (!strcasecmp(name, "MAC1"))
		return show_mac(1);

	r = reg_find(name);
	if (r)
		return show_reg(r);

	f = field_find(name);
	if (!f) {
		fprintf(stderr,
			"error: unknown fuse field or register '%s' (run 'imx-sectool fusemap' for the list)\n",
			name);
		return EX_USAGE;
	}
	rc = fuse_word(f->bank, f->word, &w);
	if (rc == FIELD_UNAVAIL)
		return EX_UNAVAIL;
	if (opt_json) {
		first_json = 1;
		field_line(f);
		printf("\n");
		return EX_OK;
	}
	bits(f, b, sizeof(b));
	printf("%s: bank %u word %u bits %s\n", f->name, f->bank, f->word, b);
	if (rc) {
		printf("  read-protected\n");
		return EX_OK;
	}
	v = (w >> f->shift) & FIELD_MASK(f->width);
	m = field_meaning(f, v, mb, sizeof(mb));
	printf("  value 0x%x%s%s%s  (word 0x%08x)\n", v, m ? " (" : "",
	       m ? m : "", m ? ")" : "", w);
	return EX_OK;
}
