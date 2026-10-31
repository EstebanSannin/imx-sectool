// SPDX-License-Identifier: MIT
/*
 * imx-sectool -- 'hab_status': U-Boot's HABv4 status report, from Linux.
 *
 * U-Boot asks the Boot ROM (report_status / report_event, through TF-A on
 * i.MX8M). From Linux we read the same data the ROM keeps instead:
 *
 *   Secure boot    SEC_CONFIG fuse, as imx_hab_is_enabled() does
 *   Configuration  0xcc closed / 0xf0 open, from the same fuse
 *   State          SNVS HPSR SSM state, which the ROM reports as HAB state
 *   Events         the ROM's HAB persistent area in OCRAM: an EVT_DEF record
 *                  (tag 0x0C) followed by event records (tag 0xDB), the same
 *                  bytes report_event() copies out (verified against U-Boot on
 *                  i.MX8M Plus, docs/phase0-imx8mp.md; same layout on
 *                  i.MX7D)
 *
 * Output and exit code follow U-Boot with the meta-toradex-security patch:
 * exit 1 when any event is present.
 *
 * Author: Stefano Viola <stefano.viola@toradex.com>
 */
#include <string.h>

#include "sectool.h"

#define HAB_TAG_EVT 0xDB
#define HAB_TAG_EVT_DEF 0x0C
#define HAB_MAJ_MASK 0xF0
#define HAB_MAJ_VER 0x40
#define MAX_EVENT_BYTES 128
#define MAX_EVENTS 32

#define HAB_CFG_OPEN 0xf0
#define HAB_CFG_CLOSED 0xcc

#define SNVS_HPSR 0x14

/* ---- U-Boot's decoding tables (arch/arm/mach-imx/hab.c) ------------------- */

struct code {
	uint8_t v;
	const char *s;
};

static const struct code sts[] = { { 0x00, "STS = HAB_STS_ANY (0x00)" },
				   { 0x33, "STS = HAB_FAILURE (0x33)" },
				   { 0x69, "STS = HAB_WARNING (0x69)" },
				   { 0xf0, "STS = HAB_SUCCESS (0xF0)" },
				   { 0, NULL } };
static const struct code rsn[] = { { 0x00, "RSN = HAB_RSN_ANY (0x00)" },
				   { 0x30, "RSN = HAB_ENG_FAIL (0x30)" },
				   { 0x22, "RSN = HAB_INV_ADDRESS (0x22)" },
				   { 0x0c, "RSN = HAB_INV_ASSERTION (0x0C)" },
				   { 0x28, "RSN = HAB_INV_CALL (0x28)" },
				   { 0x21, "RSN = HAB_INV_CERTIFICATE (0x21)" },
				   { 0x06, "RSN = HAB_INV_COMMAND (0x06)" },
				   { 0x11, "RSN = HAB_INV_CSF (0x11)" },
				   { 0x27, "RSN = HAB_INV_DCD (0x27)" },
				   { 0x0f, "RSN = HAB_INV_INDEX (0x0F)" },
				   { 0x05, "RSN = HAB_INV_IVT (0x05)" },
				   { 0x1d, "RSN = HAB_INV_KEY (0x1D)" },
				   { 0x1e, "RSN = HAB_INV_RETURN (0x1E)" },
				   { 0x18, "RSN = HAB_INV_SIGNATURE (0x18)" },
				   { 0x17, "RSN = HAB_INV_SIZE (0x17)" },
				   { 0x2e, "RSN = HAB_MEM_FAIL (0x2E)" },
				   { 0x2b, "RSN = HAB_OVR_COUNT (0x2B)" },
				   { 0x2d, "RSN = HAB_OVR_STORAGE (0x2D)" },
				   { 0x12, "RSN = HAB_UNS_ALGORITHM (0x12)" },
				   { 0x03, "RSN = HAB_UNS_COMMAND (0x03)" },
				   { 0x0a, "RSN = HAB_UNS_ENGINE (0x0A)" },
				   { 0x24, "RSN = HAB_UNS_ITEM (0x24)" },
				   { 0x1b, "RSN = HAB_UNS_KEY (0x1B)" },
				   { 0x14, "RSN = HAB_UNS_PROTOCOL (0x14)" },
				   { 0x09, "RSN = HAB_UNS_STATE (0x09)" },
				   { 0, NULL } };
static const struct code ctx[] = { { 0x00, "CTX = HAB_CTX_ANY(0x00)" },
				   { 0xff, "CTX = HAB_CTX_FAB (0xFF)" },
				   { 0xe1, "CTX = HAB_CTX_ENTRY (0xE1)" },
				   { 0x33, "CTX = HAB_CTX_TARGET (0x33)" },
				   { 0x0a,
				     "CTX = HAB_CTX_AUTHENTICATE (0x0A)" },
				   { 0xdd, "CTX = HAB_CTX_DCD (0xDD)" },
				   { 0xcf, "CTX = HAB_CTX_CSF (0xCF)" },
				   { 0xc0, "CTX = HAB_CTX_COMMAND (0xC0)" },
				   { 0xdb, "CTX = HAB_CTX_AUT_DAT (0xDB)" },
				   { 0xa0, "CTX = HAB_CTX_ASSERT (0xA0)" },
				   { 0xee, "CTX = HAB_CTX_EXIT (0xEE)" },
				   { 0, NULL } };
static const struct code eng[] = { { 0x00, "ENG = HAB_ENG_ANY (0x00)" },
				   { 0x03, "ENG = HAB_ENG_SCC (0x03)" },
				   { 0x05, "ENG = HAB_ENG_RTIC (0x05)" },
				   { 0x06, "ENG = HAB_ENG_SAHARA (0x06)" },
				   { 0x0a, "ENG = HAB_ENG_CSU (0x0A)" },
				   { 0x0c, "ENG = HAB_ENG_SRTC (0x0C)" },
				   { 0x1b, "ENG = HAB_ENG_DCP (0x1B)" },
				   { 0x1d, "ENG = HAB_ENG_CAAM (0x1D)" },
				   { 0x1e, "ENG = HAB_ENG_SNVS (0x1E)" },
				   { 0x21, "ENG = HAB_ENG_OCOTP (0x21)" },
				   { 0x22, "ENG = HAB_ENG_DTCP (0x22)" },
				   { 0x36, "ENG = HAB_ENG_ROM (0x36)" },
				   { 0x24, "ENG = HAB_ENG_HDCP (0x24)" },
				   { 0x77, "ENG = HAB_ENG_RTL (0x77)" },
				   { 0xff, "ENG = HAB_ENG_SW (0xFF)" },
				   { 0, NULL } };

static const char *lookup(const struct code *t, uint8_t v, const char *invalid)
{
	for (; t->s; t++)
		if (t->v == v)
			return t->s;
	return invalid;
}

/* short name of a status ('s'), reason ('r') or context ('c') code */
const char *hab_code_name(char kind, uint8_t v)
{
	const struct code *t = kind == 's' ? sts : kind == 'r' ? rsn : ctx;
	const char *s = lookup(t, v, NULL);

	return s ? s + 6 : NULL; /* skip "STS = " */
}

/* ---- HAB state from the SNVS security state machine ---------------------- */

static uint8_t hab_state_from_ssm(unsigned int ssm)
{
	switch (ssm) {
	case 0x0:
	case 0x8:
		return 0x33; /* init / init intermediate: INITIAL */
	case 0x9:
		return 0x55; /* check */
	case 0xb:
		return 0x66; /* non-secure */
	case 0xd:
		return 0x99; /* trusted */
	case 0xf:
		return 0xaa; /* secure */
	case 0x3:
		return 0xcc; /* soft fail */
	case 0x1:
		return 0xff; /* hard fail */
	default:
		return 0xf0; /* none */
	}
}

/* ---- HAB persistent area ------------------------------------------------- */

struct hab_event {
	uint8_t data[MAX_EVENT_BYTES];
	size_t len;
};

static size_t rec_len(const uint8_t *r)
{
	return ((size_t)r[1] << 8) | r[2];
}

/* Collect the event records. Returns the count, or -1 if the area does not
 * hold a valid EVT_DEF record (overwritten, or a different ROM layout).
 */
static int read_events(struct hab_event *ev, int max, const char **why)
{
	static uint8_t area[0x1000];
	uint32_t size = soc->hab_persist_size;
	size_t align = soc->hab_evt_align ? soc->hab_evt_align : 8;
	size_t def_len, off, l;
	const uint8_t *r;
	uint8_t ver;
	int n = 0;

	if (size > sizeof(area))
		size = sizeof(area);
	if (mmio_read(soc->hab_persist, area, size)) {
		*why = mmio_error();
		return -1;
	}

	ver = area[3];
	def_len = rec_len(area);
	if (area[0] != HAB_TAG_EVT_DEF || (ver & HAB_MAJ_MASK) != HAB_MAJ_VER ||
	    def_len < 4 || def_len >= size) {
		*why = "no HAB EVT_DEF record at the expected address";
		return -1;
	}

	/*
	 * Other record types sit between EVT_DEF and the events: find the first
	 * event record, then walk the contiguous list (aligned per SoC).
	 */
	for (off = (def_len + 3) & ~(size_t)3; off + 4 <= size; off += 4) {
		l = rec_len(&area[off]);
		if (area[off] == HAB_TAG_EVT && area[off + 3] == ver &&
		    l >= 8 && l <= MAX_EVENT_BYTES)
			break;
	}

	while (off + 4 <= size && n < max) {
		r = &area[off];
		l = rec_len(r);
		if (r[0] != HAB_TAG_EVT || r[3] != ver || l < 8 ||
		    l > MAX_EVENT_BYTES || off + l > size)
			break;
		memcpy(ev[n].data, r, l);
		ev[n].len = l;
		n++;
		off = ((soc->hab_persist + off + l + align - 1) & ~(align - 1)) -
		      soc->hab_persist;
	}
	return n;
}

/* U-Boot's display_event() + process_event_record() */
static void display_event(const uint8_t *d, size_t bytes)
{
	size_t i;

	for (i = 0; i < bytes; i++) {
		if (i == 0)
			printf("\t0x%02x", d[i]);
		else if ((i % 8) == 0)
			printf("\n\t0x%02x", d[i]);
		else
			printf(" 0x%02x", d[i]);
	}
	printf("\n\n%s\n", lookup(sts, d[4], "STS = INVALID"));
	printf("%s\n", lookup(rsn, d[5], "RSN = INVALID"));
	printf("%s\n", lookup(ctx, d[6], "CTX = INVALID"));
	printf("%s\n", lookup(eng, d[7], "ENG = INVALID"));
}

int hab_event_count(const char **why)
{
	static struct hab_event ev[MAX_EVENTS];

	return read_events(ev, MAX_EVENTS, why);
}

/* ---- command ------------------------------------------------------------- */

int cmd_hab_status(int argc, char **argv)
{
	static struct hab_event ev[MAX_EVENTS];
	const char *why = NULL;
	uint8_t config, state;
	uint32_t sec, hpsr;
	int closed, n, i;

	(void)argc;
	(void)argv;

	if (nvmem_read(soc->sec_bank * soc->words_per_bank + soc->sec_word,
		       &sec) ||
	    sec == OCOTP_READ_LOCKED) {
		puts("\nSecure boot fuse read error");
		return EX_FAIL;
	}
	closed = (sec & soc->sec_mask) == soc->sec_mask;
	printf("\nSecure boot %s\n", closed ? "enabled" : "disabled");

	config = closed ? HAB_CFG_CLOSED : HAB_CFG_OPEN;
	if (mmio_read32(soc->snvs_base + SNVS_HPSR, &hpsr)) {
		printf("\nHAB Configuration: 0x%02x, HAB State: unavailable\n",
		       config);
		fflush(stdout);
		fprintf(stderr, "error: cannot read the SNVS state: %s\n",
			mmio_error());
		return EX_UNAVAIL;
	}
	state = hab_state_from_ssm((hpsr >> 8) & 0xf);
	printf("\nHAB Configuration: 0x%02x, HAB State: 0x%02x\n", config,
	       state);

	n = read_events(ev, MAX_EVENTS, &why);
	if (n < 0) {
		fprintf(stderr, "error: HAB events unavailable: %s\n", why);
		return strstr(why, "EVT_DEF") ? EX_UNTRUSTED : EX_UNAVAIL;
	}
	if (n == 0) {
		puts("No HAB Events Found!\n");
		return EX_OK;
	}
	for (i = 0; i < n; i++) {
		puts("");
		printf("--------- HAB Event %d -----------------\n", i + 1);
		puts("event data:");
		display_event(ev[i].data, ev[i].len);
		puts("");
	}
	return EX_FAIL; /* events present (meta-toradex-security semantics) */
}
