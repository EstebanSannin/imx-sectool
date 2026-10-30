// SPDX-License-Identifier: MIT
/*
 * imx-sectool -- 'debug-status' and 'audit'.
 *
 * debug-status  The JTAG / secure-debug policy burned into the fuses, reported
 *               in the terms of meta-toradex-security's tdx-secure-debug class
 *               (TDX_SECURE_DEBUG_MODE = authenticated | no-debug, or
 *               TDX_SECURE_DEBUG_SJC_DISABLE = 1). The inference mirrors
 *               create_fuse_cmds.sh of that layer (SJC backend, i.MX6/7/8M).
 *               --expect <mode> turns it into a pass/fail check.
 *
 * audit         Secure-boot / secure-debug checklist: every check ends up as
 *               pass, warn, fail or unavailable, never silently as pass.
 *
 * Both accept --profile <imx-config.fuse>: the fuse map the layer's build
 * writes next to fuse-cmds.txt. Every row is checked against the fuses. Key
 * rows (SJC_RESP_*, OEM_KEY_*, TZ_KEY_*) are never compared by value; only
 * whether they are programmed and read-locked.
 *
 * Author: Stefano Viola <stefano.viola@toradex.com>
 */
#define _POSIX_C_SOURCE 200809L
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "sectool.h"

#define SNVS_HPSR 0x14
#define MAX_CHECKS 96

/* ---- check results ----------------------------------------------------------- */

enum status { ST_PASS, ST_WARN, ST_FAIL, ST_UNAVAIL };

static const char *const st_name[] = { "pass", "warn", "fail", "unavailable" };
static const char *const st_tag[] = { "[PASS]", "[WARN]", "[FAIL]", "[ N/A]" };

struct check {
	enum status st;
	char id[48];
	char title[64];
	char detail[160];
};

static struct check checks[MAX_CHECKS];
static int n_checks;

static void add(enum status st, const char *id, const char *title,
		const char *fmt, ...) __attribute__((format(printf, 4, 5)));

static void add(enum status st, const char *id, const char *title,
		const char *fmt, ...)
{
	struct check *c;
	va_list ap;

	if (n_checks >= MAX_CHECKS)
		return;
	c = &checks[n_checks++];
	c->st = st;
	snprintf(c->id, sizeof(c->id), "%s", id);
	snprintf(c->title, sizeof(c->title), "%s", title);
	va_start(ap, fmt);
	vsnprintf(c->detail, sizeof(c->detail), fmt, ap);
	va_end(ap);
}

/* fail > unavailable > warn > pass */
static int exit_code(void)
{
	int f = 0, u = 0, w = 0, i;

	for (i = 0; i < n_checks; i++) {
		if (checks[i].st == ST_FAIL)
			f++;
		else if (checks[i].st == ST_UNAVAIL)
			u++;
		else if (checks[i].st == ST_WARN)
			w++;
	}
	return f ? EX_FAIL : u ? EX_UNAVAIL : w ? EX_WARN : EX_OK;
}

static const char *result_word(int rc)
{
	if (rc == EX_OK)
		return "ok";
	if (rc == EX_WARN)
		return "warn";
	if (rc == EX_UNAVAIL)
		return "unavailable";
	return "fail";
}

static void json_str(const char *s)
{
	putchar('"');
	for (; *s; s++) {
		if (*s == '"' || *s == '\\')
			putchar('\\');
		putchar(*s);
	}
	putchar('"');
}

static void print_checks_text(void)
{
	int i;

	for (i = 0; i < n_checks; i++)
		printf("  %s %-30s %s\n", st_tag[checks[i].st], checks[i].title,
		       checks[i].detail);
}

static void print_checks_json(void)
{
	int i;

	printf("\"checks\": [");
	for (i = 0; i < n_checks; i++) {
		printf("%s\n  {\"id\": ", i ? "," : "");
		json_str(checks[i].id);
		printf(", \"status\": \"%s\", \"title\": ",
		       st_name[checks[i].st]);
		json_str(checks[i].title);
		printf(", \"detail\": ");
		json_str(checks[i].detail);
		putchar('}');
	}
	printf("\n]");
}

/* ---- shared readers ---------------------------------------------------------- */

/* read a field; on failure record an 'unavailable' check and return -1 */
static int need(const char *name, uint32_t *v)
{
	int rc = field_read(name, v);

	if (rc == 0)
		return 0;
	add(ST_UNAVAIL, name, name, "%s",
	    rc == FIELD_LOCKED ? "read-protected" : "cannot read fuses");
	return -1;
}

/* SRK hash state: 0 empty, 1 programmed, -1 unreadable */
static int srk_programmed(void)
{
	unsigned int i, idx;
	int any = 0;
	uint32_t w;

	for (i = 0; i < soc->srk_words; i++) {
		idx = soc->srk_bank * soc->words_per_bank + i;
		if (fuse_word(idx / soc->words_per_bank,
			      idx % soc->words_per_bank, &w))
			return -1;
		if (w)
			any = 1;
	}
	return any;
}

/* key words: 0 empty, 1 programmed and readable, 2 read-locked, -1 error */
static int key_state(unsigned int bank, unsigned int word, unsigned int n)
{
	int locked = 0, any = 0, rc;
	unsigned int i;
	uint32_t w;

	for (i = 0; i < n; i++) {
		rc = fuse_word(bank, word + i, &w);
		if (rc == FIELD_LOCKED)
			locked = 1;
		else if (rc)
			return -1;
		else if (w)
			any = 1;
	}
	return locked ? 2 : any ? 1 : 0;
}

static const char *ssm_name(unsigned int ssm)
{
	switch (ssm) {
	case 0x0:
		return "init";
	case 0x8:
		return "init intermediate";
	case 0x9:
		return "check";
	case 0xb:
		return "non-secure";
	case 0xd:
		return "trusted";
	case 0xf:
		return "secure";
	case 0x3:
		return "soft fail";
	case 0x1:
		return "hard fail";
	default:
		return "unknown";
	}
}

/* ---- debug policy (SJC backend, i.MX6/7/8M) ----------------------------------- */

struct debug_policy {
	const char *
		mode; /* open | no-debug | authenticated | jtag-disabled | invalid */
	const char *layer; /* the tdx-secure-debug setting that produces it */
	uint32_t smode, sjc_disable, kte, heo, cfg_lock;
	int key; /* key_state() of SJC_RESP */
};

static const char *smode_name(uint32_t v)
{
	switch (v) {
	case 0:
		return "JTAG enabled";
	case 1:
		return "Secure JTAG";
	case 3:
		return "No debug";
	default:
		return "reserved";
	}
}

/* Read the SJC fuses and record debug-policy checks. Returns -1 if fuses are
 * unreadable (checks already record why).
 */
static int debug_checks(struct debug_policy *p)
{
	if (need("JTAG_SMODE", &p->smode) ||
	    need("SJC_DISABLE", &p->sjc_disable) || need("KTE", &p->kte) ||
	    need("JTAG_HEO", &p->heo) || need("BOOT_CFG_LOCK", &p->cfg_lock))
		return -1;
	p->key = key_state(8, 0, 2); /* SJC_RESP: bank 8 words 0-1 on i.MX8M */

	if (p->sjc_disable) {
		p->mode = "jtag-disabled";
		p->layer = "TDX_SECURE_DEBUG_SJC_DISABLE=\"1\"";
	} else if (p->smode == 3) {
		p->mode = "no-debug";
		p->layer = "TDX_SECURE_DEBUG_MODE=\"no-debug\"";
	} else if (p->smode == 1) {
		p->mode = "authenticated";
		p->layer = "TDX_SECURE_DEBUG_MODE=\"authenticated\"";
	} else if (p->smode == 0) {
		p->mode = "open";
		p->layer = "secure debug not provisioned";
	} else {
		p->mode = "invalid";
		p->layer = "none (reserved JTAG_SMODE value)";
	}

	if (!strcmp(p->mode, "open")) {
		add(ST_FAIL, "debug-policy", "JTAG / debug policy",
		    "JTAG fully open (JTAG_SMODE=00, SJC_DISABLE=0)");
	} else if (!strcmp(p->mode, "invalid")) {
		add(ST_FAIL, "debug-policy", "JTAG / debug policy",
		    "reserved JTAG_SMODE=%u%u", (p->smode >> 1) & 1,
		    p->smode & 1);
	} else if (!strcmp(p->mode, "authenticated") && !soc->secure_jtag) {
		add(ST_FAIL, "debug-policy", "JTAG / debug policy",
		    "Secure JTAG is defeatured on %s (ERR052318); this mode is not enforced",
		    soc->name);
	} else {
		add(ST_PASS, "debug-policy", "JTAG / debug policy", "%s",
		    p->mode);
	}

	if (strcmp(p->mode, "open") && strcmp(p->mode, "jtag-disabled") &&
	    !p->kte)
		add(ST_WARN, "debug-kte", "Trace gated (KTE)",
		    "KTE=0: bus trace is not gated by the debug policy");

	if (!strcmp(p->mode, "authenticated")) {
		if (p->key == 0)
			add(ST_FAIL, "debug-key", "SJC response key",
			    "not programmed");
		else if (p->key == 1)
			add(ST_FAIL, "debug-key", "SJC response key",
			    "programmed but NOT read-locked: readable by any root process");
		else if (p->key == 2)
			add(ST_PASS, "debug-key", "SJC response key",
			    "programmed, read-locked");
		else
			add(ST_UNAVAIL, "debug-key", "SJC response key",
			    "cannot read");
		if (!p->heo)
			add(ST_WARN, "debug-heo", "HAB JTAG reopen blocked",
			    "JTAG_HEO=0: trusted boot code can reopen JTAG");
	} else if (p->key == 1) {
		add(ST_WARN, "debug-key", "SJC response key",
		    "programmed but NOT read-locked");
	}

	if (strcmp(p->mode, "open")) {
		if (p->cfg_lock & 2)
			add(ST_PASS, "debug-lock", "JTAG mode locked",
			    "BOOT_CFG_LOCK has override-protect");
		else
			add(ST_WARN, "debug-lock", "JTAG mode locked",
			    "BOOT_CFG_LOCK=%u: JTAG mode not override-protected",
			    p->cfg_lock);
	}
	return 0;
}

/* ---- profile: meta-toradex-security imx-config.fuse ------------------------ */

/* secret key words (SJC_RESP_LO, OEM_KEY_0, ...), not their *_LOCK bits */
static int is_key_row(const char *name)
{
	size_t n = strlen(name);

	if (n >= 5 && !strcmp(name + n - 5, "_LOCK"))
		return 0;
	return !strncmp(name, "SJC_RESP", 8) || !strncmp(name, "OEM_KEY_", 8) ||
	       !strncmp(name, "TZ_KEY_", 7);
}

/* Check every row of the profile; debug_only keeps SJC/ELE rows only. */
static int profile_checks(const char *path, int debug_only)
{
	unsigned int bank, word;
	char id[48], title[64];
	int lineno = 0, nt, hab, rc, exact, ok;
	char line[256], *tok[6], *s;
	const char *name;
	uint32_t want, v;
	FILE *f;

	f = fopen(path, "r");
	if (!f) {
		fprintf(stderr, "error: cannot open profile %s\n", path);
		return -1;
	}
	while (fgets(line, sizeof(line), f)) {
		lineno++;
		line[strcspn(line, "\r\n")] = '\0';
		if (!line[0] || line[0] == '#')
			continue;

		/*
		 * H:T:<type> | H:F:b:w:v | H:C:b:w:v | SJC:NAME:b:w:v | ELE:NAME:b:w:v
		 * Split on ':' keeping empty fields.
		 */
		memset(tok, 0, sizeof(tok));
		nt = 0;
		for (s = line; s && nt < 6;) {
			tok[nt++] = s;
			s = strchr(s, ':');
			if (s)
				*s++ = '\0';
		}
		if (nt < 3)
			continue;
		if (!strcmp(tok[0], "H") && !strcmp(tok[1], "T"))
			continue;

		hab = !strcmp(tok[0], "H");
		if (debug_only && hab)
			continue;
		if (nt < 5 ||
		    (hab && strcmp(tok[1], "F") && strcmp(tok[1], "C"))) {
			add(ST_WARN, "profile", "profile row",
			    "line %d not understood", lineno);
			continue;
		}

		bank = (unsigned int)strtoul(tok[2], NULL, 0);
		word = (unsigned int)strtoul(tok[3], NULL, 0);
		want = (uint32_t)strtoul(tok[4], NULL, 0);
		if (hab)
			name = !strcmp(tok[1], "F") ? "SRK_HASH" : "CLOSE";
		else
			name = tok[1];
		snprintf(id, sizeof(id), "profile:%s:%s", tok[0], name);
		snprintf(title, sizeof(title), "%s:%s (%u/%u)", tok[0], name,
			 bank, word);

		if (!strcmp(tok[0], "ELE")) {
			add(ST_UNAVAIL, id, title,
			    "ELE fuses are not supported on %s", soc->name);
			continue;
		}
		if (bank >= soc->banks || word >= soc->words_per_bank) {
			add(ST_FAIL, id, title, "bank/word out of range for %s",
			    soc->name);
			continue;
		}

		rc = fuse_word(bank, word, &v);
		if (is_key_row(name)) {
			if (rc == FIELD_LOCKED)
				add(ST_PASS, id, title,
				    "programmed, read-locked (value not compared)");
			else if (rc)
				add(ST_UNAVAIL, id, title, "cannot read fuses");
			else if (v)
				add(ST_WARN, id, title,
				    "programmed but readable (value not compared)");
			else
				add(ST_FAIL, id, title, "not programmed");
			continue;
		}
		if (rc) {
			add(ST_UNAVAIL, id, title, "%s",
			    rc == FIELD_LOCKED ? "read-protected" :
						 "cannot read fuses");
			continue;
		}
		/* SRK hash words must match exactly; everything else is a bit mask */
		exact = hab && !strcmp(tok[1], "F");
		ok = exact ? v == want : (v & want) == want;
		if (ok)
			add(ST_PASS, id, title, "0x%08x as expected", v);
		else
			add(ST_FAIL, id, title, "is 0x%08x, expected %s0x%08x",
			    v, exact ? "" : "bits ", want);
	}
	fclose(f);
	return 0;
}

/* ---- option parsing ----------------------------------------------------------- */

static const char *opt_value(int argc, char **argv, const char *opt)
{
	int i;

	for (i = 1; i < argc - 1; i++)
		if (!strcmp(argv[i], opt))
			return argv[i + 1];
	return NULL;
}

static int has_unknown_args(int argc, char **argv, const char *const *known)
{
	const char *const *k;
	int i, ok;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--json"))
			continue;
		ok = 0;
		for (k = known; *k; k++)
			if (!strcmp(argv[i], *k) && i + 1 < argc) {
				ok = 1;
				i++;
				break;
			}
		if (!ok) {
			fprintf(stderr, "error: unexpected argument '%s'\n",
				argv[i]);
			return 1;
		}
	}
	return 0;
}

/* ---- debug-status --------------------------------------------------------------- */

static const char *const debug_modes[] = { "open", "no-debug", "authenticated",
					   "jtag-disabled", NULL };

static const char *boot_word(int known, uint32_t closed)
{
	if (!known)
		return "unknown";
	return closed ? "closed" : "open";
}

static const char *cfg_lock_word(uint32_t v)
{
	static const char *const names[] = { "00 (unlocked)",
					     "01 (write-protect)",
					     "10 (override-protect)",
					     "11 (WP + OP)" };

	return names[v & 3];
}

static const char *key_word(int key)
{
	switch (key) {
	case 2:
		return "programmed, read-locked";
	case 1:
		return "programmed, READABLE";
	case 0:
		return "not programmed";
	default:
		return "unreadable";
	}
}

int cmd_debug_status(int argc, char **argv)
{
	static const char *const known[] = { "--expect", "--profile", NULL };
	struct debug_policy p = { 0 };
	const char *expect, *profile;
	const char *const *m;
	int have, sec_ok, match, ok, rc, i;
	uint32_t closed = 0;
	char smode[32];

	if (has_unknown_args(argc, argv, known))
		return EX_USAGE;
	expect = opt_value(argc, argv, "--expect");
	profile = opt_value(argc, argv, "--profile");

	if (expect) {
		ok = 0;
		for (m = debug_modes; *m; m++)
			if (!strcmp(*m, expect))
				ok = 1;
		if (!ok) {
			fprintf(stderr,
				"error: --expect takes open, no-debug, authenticated or jtag-disabled\n");
			return EX_USAGE;
		}
	}

	n_checks = 0;
	have = debug_checks(&p) == 0;
	sec_ok = field_read("SEC_CONFIG[1]", &closed) == 0;

	if (have && expect) {
		match = !strcmp(expect, p.mode);
		/* an open JTAG is only a failure when nobody asked for it */
		if (match && !strcmp(p.mode, "open"))
			for (i = 0; i < n_checks; i++)
				if (!strcmp(checks[i].id, "debug-policy")) {
					checks[i].st = ST_PASS;
					snprintf(checks[i].detail,
						 sizeof(checks[i].detail),
						 "open, as expected");
				}
		if (match)
			add(ST_PASS, "expect", "Expected mode", "%s", expect);
		else
			add(ST_FAIL, "expect", "Expected mode",
			    "expected %s, fuses say %s", expect, p.mode);
	}
	if (profile && profile_checks(profile, 1))
		return EX_USAGE;
	rc = exit_code();

	if (opt_json) {
		printf("{\"soc\": \"%s\", \"backend\": \"sjc\", ", soc->soc_id);
		if (have) {
			printf("\"mode\": \"%s\", \"layer\": ", p.mode);
			json_str(p.layer);
			printf(", \"fuses\": {\"JTAG_SMODE\": %u, \"SJC_DISABLE\": %u, \"KTE\": %u, \"JTAG_HEO\": %u, \"BOOT_CFG_LOCK\": %u}, ",
			       p.smode, p.sjc_disable, p.kte, p.heo,
			       p.cfg_lock);
		}
		printf("\"secure_boot\": \"%s\", ", boot_word(sec_ok, closed));
		print_checks_json();
		printf(", \"result\": \"%s\"}\n", result_word(rc));
		return rc;
	}

	printf("SoC:              %s (SJC backend)\n", soc->name);
	if (have) {
		printf("Secure debug:     %-21s %s\n", p.mode, p.layer);
		snprintf(smode, sizeof(smode), "%u%u (%s)", (p.smode >> 1) & 1,
			 p.smode & 1, smode_name(p.smode));
		printf("  JTAG_SMODE:     %-21s bank 1 word 3\n", smode);
		printf("  SJC_DISABLE:    %-21u bank 1 word 3\n",
		       p.sjc_disable);
		printf("  KTE:            %-21s bank 1 word 3\n",
		       p.kte ? "1 (trace gated)" : "0");
		if (soc->secure_jtag)
			printf("  JTAG_HEO:       %-21u bank 1 word 3\n",
			       p.heo);
		printf("  BOOT_CFG_LOCK:  %-21s bank 0 word 0\n",
		       cfg_lock_word(p.cfg_lock));
		if (p.key > 0 || soc->secure_jtag)
			printf("  SJC_RESP:       %s\n", key_word(p.key));
	}
	printf("Secure boot:      %s\n", boot_word(sec_ok, closed));
	if (n_checks) {
		printf("Checks:\n");
		print_checks_text();
	}
	printf("Result:           %s\n", result_word(rc));
	return rc;
}

/* ---- audit ------------------------------------------------------------------------ */

static enum status ssm_status(unsigned int ssm)
{
	if (ssm == 0xd || ssm == 0xf)
		return ST_PASS;		/* trusted, secure */
	if (ssm == 0x3 || ssm == 0x1)
		return ST_FAIL;		/* soft fail, hard fail */
	return ST_WARN;
}

int cmd_audit(int argc, char **argv)
{
	static const char *const known[] = { "--profile", NULL };
	struct debug_policy p = { 0 };
	const char *why = NULL;
	const char *profile;
	int cnt[4] = { 0 };
	int ev, srk, rc, i;
	unsigned int ssm;
	uint32_t v, w;

	if (has_unknown_args(argc, argv, known))
		return EX_USAGE;
	profile = opt_value(argc, argv, "--profile");

	n_checks = 0;

	/* 1. secure boot closed */
	if (need("SEC_CONFIG[1]", &v) == 0) {
		if (v)
			add(ST_PASS, "secure-boot", "Secure boot closed",
			    "SEC_CONFIG[1]=1");
		else
			add(ST_FAIL, "secure-boot", "Secure boot closed",
			    "SEC_CONFIG[1]=0: unsigned images boot");
	}

	/* 2. HAB events */
	ev = hab_event_count(&why);
	if (ev < 0)
		add(ST_UNAVAIL, "hab-events", "No HAB events", "%s", why);
	else if (ev == 0)
		add(ST_PASS, "hab-events", "No HAB events",
		    "none logged by the Boot ROM");
	else
		add(ST_FAIL, "hab-events", "No HAB events",
		    "%d event(s): run 'hab_status'", ev);

	/* 3-4. SRK hash programmed and locked */
	srk = srk_programmed();
	if (srk < 0)
		add(ST_UNAVAIL, "srk-hash", "SRK hash programmed",
		    "cannot read fuses");
	else if (srk)
		add(ST_PASS, "srk-hash", "SRK hash programmed",
		    "bank %u, %u words", soc->srk_bank, soc->srk_words);
	else
		add(ST_FAIL, "srk-hash", "SRK hash programmed", "all zero");
	if (need(soc->srk_lock, &v) == 0) {
		if (v)
			add(ST_PASS, "srk-lock", "SRK hash locked", "%s=1",
			    soc->srk_lock);
		else
			add(srk == 1 ? ST_FAIL : ST_WARN, "srk-lock",
			    "SRK hash locked",
			    "%s=0: the SRK hash words can still be burned",
			    soc->srk_lock);
	}

	/* 5. SRK revocation (informational) */
	if (need("SRK_REVOKE", &v) == 0) {
		if (v == FIELD_MASK(field_find("SRK_REVOKE")->width))
			add(ST_FAIL, "srk-revoke", "SRK keys usable",
			    "all revocable SRK keys revoked");
		else if (v)
			add(ST_WARN, "srk-revoke", "SRK keys usable",
			    "revoked mask 0x%x", v);
		else
			add(ST_PASS, "srk-revoke", "SRK keys usable",
			    "none revoked");
	}

	/* 6-8. debug policy */
	debug_checks(&p);

	/* 9. serial downloader */
	if (need("SDP_DISABLE", &v) == 0) {
		if (v)
			add(ST_PASS, "sdp", "Serial downloader disabled",
			    "SDP_DISABLE=1");
		else
			add(ST_WARN, "sdp", "Serial downloader disabled",
			    "SDP_DISABLE=0: USB recovery boot possible (signed images only once closed)");
	}

	/* 10. boot from fuses */
	if (need("BT_FUSE_SEL", &v) == 0) {
		if (v)
			add(ST_PASS, "bt-fuse-sel", "Boot config from fuses",
			    "BT_FUSE_SEL=1");
		else
			add(ST_WARN, "bt-fuse-sel", "Boot config from fuses",
			    "BT_FUSE_SEL=0: boot pins decide");
	}

	/* 11. field return */
	if (need("FIELD_RETURN", &v) == 0) {
		if (v)
			add(ST_WARN, "field-return", "Not in field-return",
			    "FIELD_RETURN=1");
		else
			add(ST_PASS, "field-return", "Not in field-return",
			    "FIELD_RETURN=0");
	}

	/* 12. SNVS security state */
	if (mmio_read32(soc->snvs_base + SNVS_HPSR, &w)) {
		add(ST_UNAVAIL, "ssm", "SNVS security state", "%s",
		    mmio_error());
	} else {
		ssm = (w >> 8) & 0xf;
		add(ssm_status(ssm), "ssm", "SNVS security state",
		    "%s (SSM_STATE 0x%x)", ssm_name(ssm), ssm);
	}

	if (profile && profile_checks(profile, 0))
		return EX_USAGE;
	rc = exit_code();

	if (opt_json) {
		printf("{\"soc\": \"%s\", ", soc->soc_id);
		print_checks_json();
		printf(", \"result\": \"%s\"}\n", result_word(rc));
		return rc;
	}

	for (i = 0; i < n_checks; i++)
		cnt[checks[i].st]++;
	printf("imx-sectool audit - %s\n", soc->name);
	print_checks_text();
	printf("Result: %s (passed %d, warnings %d, failed %d, unavailable %d)\n",
	       result_word(rc), cnt[ST_PASS], cnt[ST_WARN], cnt[ST_FAIL],
	       cnt[ST_UNAVAIL]);
	return rc;
}
