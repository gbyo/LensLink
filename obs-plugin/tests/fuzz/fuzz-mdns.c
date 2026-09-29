/*
 * Untrusted mDNS replies through the real parser, one packet and then a
 * second from the same address (the merge path). Invariants: every
 * result field is terminated valid UTF-8, an id is absent or canonical,
 * and the count never exceeds its bound.
 */

#include "fuzz.h"

#include "mdns-parse.h"

#include <string.h>

/* PTR + TXT for "Emma's iPhone", compressed the way mDNSResponder does. */
#define REPLY                                                              \
	"\x00\x00\x84\x00\x00\x00\x00\x02\x00\x00\x00\x00"                  \
	"\x09_lenslink\x04_tcp\x05local\x00"                                \
	"\x00\x0c\x80\x01\x00\x00\x11\x94\x00\x10"                          \
	"\x0d" "Emma's iPhone\xc0\x0c"                                      \
	"\xc0\x2c"                                                          \
	"\x00\x10\x80\x01\x00\x00\x11\x94\x00\x3c"                          \
	"\x27id=B1092868-1234-5678-ABCD-0123456789AB"                       \
	"\x07proto=1"                                                       \
	"\x0bkind=camera"

static const struct fuzz_seed seeds[] = {
	FUZZ_SEED(REPLY),
	/* PTR only, no TXT (an older app). */
	FUZZ_SEED("\x00\x00\x84\x00\x00\x00\x00\x01\x00\x00\x00\x00"
		  "\x09_lenslink\x04_tcp\x05local\x00"
		  "\x00\x0c\x80\x01\x00\x00\x11\x94\x00\x08"
		  "\x05Phone\xc0\x0c"),
	/* A question echoed back, then a compression loop. */
	FUZZ_SEED("\x00\x00\x84\x00\x00\x01\x00\x01\x00\x00\x00\x00"
		  "\x09_lenslink\x04_tcp\x05local\x00\x00\x0c\x00\x01"
		  "\xc0\x2e\x00\x0c\x00\x01\x00\x00\x00\x01\x00\x02\xc0\x2e"),
};

const struct fuzz_seed *fuzz_seeds(size_t *count)
{
	*count = sizeof(seeds) / sizeof(seeds[0]);
	return seeds;
}

static void check(const struct mdns_result *r, int n, int max)
{
	FUZZ_ASSERT(n >= 0 && n <= max);
	for (int i = 0; i < n; i++) {
		FUZZ_ASSERT(fuzz_utf8_field_ok(r[i].name, sizeof(r[i].name)));
		FUZZ_ASSERT(fuzz_utf8_field_ok(r[i].host, sizeof(r[i].host)));
		FUZZ_ASSERT(r[i].device_id[0] == 0 ||
			    lenslink_device_id_valid(r[i].device_id));
		FUZZ_ASSERT(r[i].protocol >= 0 && r[i].protocol <= 255);
		/* A control character never survives into a name. */
		for (const char *p = r[i].name; *p; p++)
			FUZZ_ASSERT((unsigned char)*p >= 0x20 && *p != 0x7F);
	}
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	uint8_t service[64];
	int slen = mdns_encode_name("_lenslink._tcp.local", service,
				    sizeof(service));
	FUZZ_ASSERT(slen > 0);

	struct mdns_result r[3];
	memset(r, 0, sizeof(r));
	int n = mdns_parse_response(data, size, service, (size_t)slen,
				    "192.168.1.47", r, 0, 3);
	check(r, n, 3);
	n = mdns_parse_response(data, size, service, (size_t)slen,
				"192.168.1.47", r, n, 3);
	check(r, n, 3);
	n = mdns_parse_response(data, size, service, (size_t)slen,
				"192.168.1.48", r, n, 3);
	check(r, n, 3);

	/* The TXT parser on the raw bytes too. */
	struct mdns_result t;
	memset(&t, 0, sizeof(t));
	if (mdns_parse_txt(data, size, &t))
		FUZZ_ASSERT(t.device_id[0] == 0 ||
			    lenslink_device_id_valid(t.device_id));

	/* And the id validator on arbitrary bytes. */
	char id[LENSLINK_DEVICE_ID_SIZE];
	if (lenslink_device_id_parse((const char *)data, size, id))
		FUZZ_ASSERT(size == LENSLINK_DEVICE_ID_LEN &&
			    lenslink_device_id_valid(id));
	else
		FUZZ_ASSERT(id[0] == 0);
	return 0;
}
