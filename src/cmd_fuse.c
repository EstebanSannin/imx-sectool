// SPDX-License-Identifier: MIT
/*
 * imx-sectool -- 'fuse': the U-Boot fuse command, from Linux.
 *
 * Syntax, messages and exit codes follow U-Boot's cmd/fuse.c including the
 * meta-toradex-security patches (-q switch, readv), so existing scripts and
 * log parsers keep working. Differences, all on the safe side:
 *
 *   prog      shows current/requested/result and the bits that will burn,
 *             skips the write when nothing would change, verifies after,
 *             and logs to syslog (LOG_AUTH).
 *   readv     there is no U-Boot environment: prints name='w0 w1' for eval.
 *   readm     there is no memory to fill: prints the words instead.
 *   sense,    need direct OCOTP register access, which Linux makes unsafe
 *   override  (it gates the OCOTP clock); reported as unavailable.
 *
 * Author: Stefano Viola <stefano.viola@toradex.com>
 */
#define _POSIX_C_SOURCE 200809L
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <syslog.h>

#include "sectool.h"

#define CMD_RET_FAILURE EX_FAIL
#define CMD_RET_USAGE EX_USAGE

static int quiet;

#define IF_NOT_QUIET(stmt)    \
	do {                  \
		if (!quiet) { \
			stmt; \
		}             \
	} while (0)

static void usage(FILE *f)
{
	fprintf(f,
		"usage: fuse read [-q] <bank> <word> [<cnt>] - read 1 or 'cnt' fuse words,\n"
		"           starting at 'word'\n"
		"       fuse readm [-q] <bank> <word> <addr> [<cnt>] - read 1 or 'cnt' fuse words\n"
		"           (Linux: printed, 'addr' is ignored)\n"
		"       fuse readv [-q] <bank> <word> <name> [<cnt>] - read 1 or 'cnt' fuse words\n"
		"           (Linux: printed as name='w0 w1 ...' for eval)\n"
		"       fuse cmp [-q] <bank> <word> <hexval> - compare 'hexval' to fuse\n"
		"           at 'word'\n"
		"       fuse sense [-q] <bank> <word> [<cnt>] - not available from Linux\n"
		"       fuse prog [-q] [-y] <bank> <word> <hexval> [<hexval>...] - program 1 or\n"
		"           several fuse words, starting at 'word' (PERMANENT)\n"
		"       fuse override [-q] <bank> <word> <hexval> [<hexval>...] - not available\n"
		"           from Linux\n"
		"\n"
		"imx-sectool extension: a fuse field name instead of <bank> <word>\n"
		"       fuse read|readv|cmp|prog [-q] [-y] <FIELD> [<name>|<value>]\n"
		"           e.g. 'fuse read JTAG_SMODE'; <value> is the field value\n"
		"           ('imx-sectool fusemap' lists the fields)\n");
}

static int strtou32(const char *str, int base, uint32_t *result)
{
	unsigned long v;
	char *ep;

	errno = 0;
	v = strtoul(str, &ep, base);
	if (ep == str || *ep != '\0' || errno || v > UINT32_MAX)
		return -EINVAL;
	*result = (uint32_t)v;
	return 0;
}

/* ---- OCOTP word access, with mxc_ocotp's error messages ------------------ */

static int check_args(uint32_t bank, uint32_t word, const char *caller)
{
	if (bank >= soc->banks || word >= soc->words_per_bank) {
		/* a driver message in U-Boot: printed even with -q */
		printf("mxc_ocotp %s(): Invalid argument\n", caller);
		return -EINVAL;
	}
	return 0;
}

static unsigned int fuse_index(uint32_t bank, uint32_t word)
{
	return bank * soc->words_per_bank + word;
}

static int fuse_read(uint32_t bank, uint32_t word, uint32_t *val)
{
	if (check_args(bank, word, "fuse_read"))
		return -EINVAL;
	if (nvmem_read(fuse_index(bank, word), val))
		return -EIO;
	if (*val == OCOTP_READ_LOCKED) {
		printf("mxc_ocotp fuse_read(): Access protect error\n");
		return -EIO;
	}
	return 0;
}

/* ---- programming ---------------------------------------------------------- */

static int confirm_prog(void)
{
	char line[16];

	puts("Warning: Programming fuses is an irreversible operation!\n"
	     "         This may brick your system.\n"
	     "         Use this command only if you are sure of what you are doing!\n"
	     "\nReally perform this fuse programming? <y/N>");
	fflush(stdout);
	if (fgets(line, sizeof(line), stdin) &&
	    (!strcmp(line, "y\n") || !strcmp(line, "Y\n") ||
	     !strcmp(line, "yes\n") || !strcmp(line, "y") ||
	     !strcmp(line, "Y")))
		return 1;
	puts("Fuse programming aborted");
	return 0;
}

/* One word: show what will actually burn, confirm, write, verify. */
static int fuse_prog(uint32_t bank, uint32_t word, uint32_t val, int confirmed)
{
	uint32_t cur, now, result, burn;

	if (check_args(bank, word, "fuse_prog"))
		return -EINVAL;
	if (nvmem_read(fuse_index(bank, word), &cur))
		return -EIO;
	if (cur == OCOTP_READ_LOCKED) {
		fprintf(stderr,
			"error: bank %u word %u is read-protected: the result cannot be verified, refusing to program it\n",
			bank, word);
		return -EPERM;
	}

	result = cur | val;
	burn = val & ~cur;
	IF_NOT_QUIET(printf(
		"  current 0x%.8x | requested 0x%.8x -> result 0x%.8x (new bits 0x%.8x)\n",
		cur, val, result, burn));
	if (!burn) {
		IF_NOT_QUIET(printf(
			"  nothing to program: all requested bits are already set\n"));
		return 0;
	}
	if (!confirmed && !confirm_prog())
		return 1; /* aborted, not an error */

	syslog(LOG_AUTH | LOG_NOTICE,
	       "fuse prog bank %u word %u: 0x%08x | 0x%08x -> 0x%08x", bank,
	       word, cur, val, result);
	if (nvmem_write(fuse_index(bank, word), val))
		return -EIO;

	/* the driver reloads the shadow registers after a write */
	if (nvmem_read(fuse_index(bank, word), &now))
		return -EIO;
	if (now != result) {
		fprintf(stderr,
			"error: verification failed: bank %u word %u reads 0x%08x, expected 0x%08x\n",
			bank, word, now, result);
		syslog(LOG_AUTH | LOG_ERR,
		       "fuse prog bank %u word %u: verify failed, 0x%08x != 0x%08x",
		       bank, word, now, result);
		return -EIO;
	}
	return 0;
}

/* ---- named fields ---------------------------------------------------------- */

/*
 * fuse read [-q] <FIELD>
 * fuse readv [-q] <FIELD> <name>
 * fuse cmp [-q] <FIELD> <value>
 * fuse prog [-q] [-y] <FIELD> <value>
 *
 * <value> is the field value, not the word: it is shifted into place. Because
 * programming can only set bits, prog refuses when bits already burned in the
 * field would make the result differ from the request.
 */
static int fuse_named(const char *op, int argc, char **argv, int confirmed)
{
	const struct fuse_field *f = field_find(argv[0]);
	uint32_t mask = FIELD_MASK(f->width), w, v, req = 0;
	char b[16], mb[64];
	const char *m;
	int need, ret;

	if (f->width == 1)
		snprintf(b, sizeof(b), "%u", f->shift);
	else
		snprintf(b, sizeof(b), "%u:%u", f->shift + f->width - 1,
			 f->shift);

	if (!strcmp(op, "sense") || !strcmp(op, "override")) {
		fprintf(stderr,
			"error: 'fuse %s' is not available from Linux\n", op);
		return EX_UNAVAIL;
	}
	need = !strcmp(op, "read") ? 1 : 2;
	if ((strcmp(op, "read") && strcmp(op, "readv") && strcmp(op, "cmp") &&
	     strcmp(op, "prog")) ||
	    argc != need) {
		usage(stderr);
		return CMD_RET_USAGE;
	}
	if (!strcmp(op, "cmp") || !strcmp(op, "prog")) {
		if (strtou32(argv[1], 0, &req) || req > mask) {
			fprintf(stderr,
				"error: value '%s' does not fit %s (%u bit%s)\n",
				argv[1], f->name, f->width,
				f->width == 1 ? "" : "s");
			return CMD_RET_USAGE;
		}
	}

	if (check_args(f->bank, f->word, "fuse_read"))
		goto err;
	if (nvmem_read(f->bank * soc->words_per_bank + f->word, &w))
		goto err;
	if (w == OCOTP_READ_LOCKED) {
		printf("mxc_ocotp fuse_read(): Access protect error\n");
		goto err;
	}
	v = (w >> f->shift) & mask;

	if (!strcmp(op, "read")) {
		m = field_meaning(f, v, mb, sizeof(mb));
		IF_NOT_QUIET(printf(
			"%s = 0x%x%s%s%s  (bank %u word %u bits %s, word 0x%08x)\n",
			f->name, v, m ? " (" : "", m ? m : "", m ? ")" : "",
			f->bank, f->word, b, w));
	} else if (!strcmp(op, "readv")) {
		IF_NOT_QUIET(
			printf("Reading %s into '%s'\n", f->name, argv[1]));
		printf("%s='%x'\n", argv[1], v);
	} else if (!strcmp(op, "cmp")) {
		IF_NOT_QUIET(printf("Comparing %s:\nValue 0x%x:0x%x\n", f->name,
				    req, v));
		if (v != req) {
			IF_NOT_QUIET(printf("failed\n"));
			return CMD_RET_FAILURE;
		}
		IF_NOT_QUIET(printf("passed\n"));
	} else {
		IF_NOT_QUIET(printf(
			"Programming %s (bank %u word %u bits %s) to 0x%x...\n",
			f->name, f->bank, f->word, b, req));
		if ((v | req) != req) {
			fflush(stdout);
			fprintf(stderr,
				"error: %s already has bits 0x%x burned; programming 0x%x would leave 0x%x, not 0x%x\n",
				f->name, v, req, v | req, req);
			return CMD_RET_FAILURE;
		}
		ret = fuse_prog(f->bank, f->word, req << f->shift, confirmed);
		if (ret > 0)
			return CMD_RET_FAILURE;
		if (ret)
			goto err;
	}
	return EX_OK;

err:
	IF_NOT_QUIET(puts("ERROR"));
	return CMD_RET_FAILURE;
}

/* ---- command ----------------------------------------------------------- */

int cmd_fuse(int argc, char **argv)
{
	const char *op = argc >= 2 ? argv[1] : NULL;
	uint32_t bank, word, cnt, val, cmp;
	unsigned long addr;
	int confirmed = 0;
	const char *name;
	char *out, *p;
	int ret, i;

	quiet = 0;
	if (!op || !strcmp(op, "-h") || !strcmp(op, "--help")) {
		usage(op ? stdout : stderr);
		return op ? EX_OK : CMD_RET_USAGE;
	}
	argc -= 2;
	argv += 2;

	/* common switches, as in the Toradex patch */
	while (argc) {
		if (!strcmp(argv[0], "-y"))
			confirmed = 1;
		else if (!strcmp(argv[0], "-q"))
			quiet = 1;
		else
			break;
		argc--;
		argv++;
	}

	/* a field name where U-Boot expects a number: imx-sectool extension */
	if (argc >= 1 && strtou32(argv[0], 0, &bank)) {
		if (field_find(argv[0]))
			return fuse_named(op, argc, argv, confirmed);
		fprintf(stderr,
			"error: '%s' is neither a bank number nor a fuse field (run 'imx-sectool fusemap' for the list)\n",
			argv[0]);
		return CMD_RET_USAGE;
	}

	if (argc < 2 || strtou32(argv[0], 0, &bank) ||
	    strtou32(argv[1], 0, &word)) {
		usage(stderr);
		return CMD_RET_USAGE;
	}

	if (!strcmp(op, "sense") || !strcmp(op, "override")) {
		fprintf(stderr,
			"error: 'fuse %s' needs direct OCOTP register access, which is not safe\n"
			"       from Linux (the kernel gates the OCOTP clock when idle).\n"
			"hint:  'fuse read' returns the burned value: the kernel reloads the\n"
			"       shadow registers after every 'fuse prog'.\n",
			op);
		return EX_UNAVAIL;
	}

	if (!strcmp(op, "read")) {
		if (argc == 2)
			cnt = 1;
		else if (argc != 3 || strtou32(argv[2], 0, &cnt)) {
			usage(stderr);
			return CMD_RET_USAGE;
		}

		IF_NOT_QUIET(printf("Reading bank %u:\n", bank));
		for (i = 0; (uint32_t)i < cnt; i++, word++) {
			if (!(i % 4))
				IF_NOT_QUIET(printf("\nWord 0x%.8x:", word));
			if (fuse_read(bank, word, &val))
				goto err;
			IF_NOT_QUIET(printf(" %.8x", val));
		}
		IF_NOT_QUIET(putchar('\n'));
	} else if (!strcmp(op, "readm")) {
		if (argc == 3)
			cnt = 1;
		else if (argc != 4 || strtou32(argv[3], 0, &cnt)) {
			usage(stderr);
			return CMD_RET_USAGE;
		}

		addr = strtoul(argv[2], NULL, 16);
		IF_NOT_QUIET(printf("Reading bank %u len %u to 0x%lx\n", bank,
				    cnt, addr));
		for (i = 0; (uint32_t)i < cnt; i++, word++) {
			if (fuse_read(bank, word, &val))
				goto err;
			printf("%.8x\n", val);
		}
	} else if (!strcmp(op, "readv")) {
		if (argc == 3)
			cnt = 1;
		else if (argc != 4 || strtou32(argv[3], 0, &cnt)) {
			usage(stderr);
			return CMD_RET_USAGE;
		}

		name = argv[2];
		IF_NOT_QUIET(printf("Reading bank %u len %u into '%s'\n", bank,
				    cnt, name));
		out = calloc(1, (size_t)cnt * 9 + 1);
		if (!out)
			goto err;
		p = out;
		for (i = 0; (uint32_t)i < cnt; i++, word++) {
			if (fuse_read(bank, word, &val)) {
				free(out);
				goto err;
			}
			p += sprintf(p, "%s%x", i ? " " : "", val);
		}
		printf("%s='%s'\n", name, out);
		free(out);
	} else if (!strcmp(op, "cmp")) {
		if (argc != 3 || strtou32(argv[2], 0, &cmp)) {
			usage(stderr);
			return CMD_RET_USAGE;
		}

		IF_NOT_QUIET(printf("Comparing bank %u:\n", bank));
		IF_NOT_QUIET(printf("\nWord 0x%.8x:", word));
		IF_NOT_QUIET(printf("\nValue 0x%.8x:", cmp));
		if (fuse_read(bank, word, &val))
			goto err;
		IF_NOT_QUIET(printf("0x%.8x\n", val));
		if (val != cmp) {
			IF_NOT_QUIET(printf("failed\n"));
			return CMD_RET_FAILURE;
		}
		IF_NOT_QUIET(printf("passed\n"));
	} else if (!strcmp(op, "prog")) {
		if (argc < 3) {
			usage(stderr);
			return CMD_RET_USAGE;
		}

		for (i = 2; i < argc; i++, word++) {
			if (strtou32(argv[i], 16, &val)) {
				usage(stderr);
				return CMD_RET_USAGE;
			}
			IF_NOT_QUIET(printf(
				"Programming bank %u word 0x%.8x to 0x%.8x...\n",
				bank, word, val));
			ret = fuse_prog(bank, word, val, confirmed);
			if (ret > 0)
				return CMD_RET_FAILURE; /* aborted by the user */
			if (ret)
				goto err;
		}
	} else {
		usage(stderr);
		return CMD_RET_USAGE;
	}
	return EX_OK;

err:
	IF_NOT_QUIET(puts("ERROR"));
	return CMD_RET_FAILURE;
}
