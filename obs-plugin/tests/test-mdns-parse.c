/* mDNS-SD reply parsing: PTR/TXT matching, TXT metadata, and hostile
 * packets. Replies are built by hand, byte for byte. */

#include "mdns-parse.h"
#include "test.h"

#include <stdint.h>
#include <stdlib.h>

#define ID_A "B1092868-1234-5678-ABCD-0123456789AB"
#define ID_B "0F5E2D4C-9A8B-4C7D-8E6F-102030405060"
#define SERVICE "_lenslink._tcp.local"

static uint8_t g_service[64];
static size_t g_service_len;

/* --- packet builder ------------------------------------------------- */

struct pkt {
	uint8_t b[1500];
	size_t n;
	unsigned records;
};

static void put8(struct pkt *p, unsigned v)
{
	p->b[p->n++] = (uint8_t)v;
}

static void put16(struct pkt *p, unsigned v)
{
	put8(p, v >> 8);
	put8(p, v & 0xFF);
}

static void put_bytes(struct pkt *p, const void *d, size_t n)
{
	memcpy(p->b + p->n, d, n);
	p->n += n;
}

/* A dotted name as labels; returns its offset (for compression). */
static size_t put_name(struct pkt *p, const char *dotted)
{
	size_t at = p->n;
	int n = mdns_encode_name(dotted, p->b + p->n, sizeof(p->b) - p->n);
	p->n += (size_t)n;
	return at;
}

/* One label (raw bytes, may hold dots/NULs), then a pointer. */
static size_t put_label_ptr(struct pkt *p, const void *label, size_t len,
			    size_t target)
{
	size_t at = p->n;
	put8(p, (unsigned)len);
	put_bytes(p, label, len);
	put16(p, 0xC000 | (unsigned)target);
	return at;
}

static void begin(struct pkt *p)
{
	memset(p, 0, sizeof(*p));
	put16(p, 0);      /* id */
	put16(p, 0x8400); /* response, authoritative */
	put16(p, 0);      /* qdcount */
	put16(p, 0);      /* ancount (patched by finish) */
	put16(p, 0);
	put16(p, 0);
}

static void finish(struct pkt *p)
{
	p->b[6] = (uint8_t)(p->records >> 8);
	p->b[7] = (uint8_t)p->records;
}

/* Record header after the owner name; returns the rdlength offset. */
static size_t rr(struct pkt *p, unsigned type, uint32_t ttl)
{
	put16(p, type);
	put16(p, 0x8001); /* cache-flush + IN */
	put16(p, ttl >> 16);
	put16(p, ttl & 0xFFFF);
	size_t rdlen_at = p->n;
	put16(p, 0);
	p->records++;
	return rdlen_at;
}

static void end_rdata(struct pkt *p, size_t rdlen_at)
{
	size_t len = p->n - rdlen_at - 2;
	p->b[rdlen_at] = (uint8_t)(len >> 8);
	p->b[rdlen_at + 1] = (uint8_t)len;
}

/* Adds a PTR for `instance` (a label) and returns the instance name's
 * offset. The service name is written once per packet (at `svc`, 0 =
 * write it now). */
static size_t ptr(struct pkt *p, size_t *svc, const char *instance,
		  uint32_t ttl)
{
	if (!*svc)
		*svc = put_name(p, SERVICE);
	else
		put16(p, 0xC000 | (unsigned)*svc);
	size_t rd = rr(p, 12, ttl);
	size_t inst = put_label_ptr(p, instance, strlen(instance), *svc);
	end_rdata(p, rd);
	return inst;
}

/* A TXT for the instance at `inst` from NUL-separated strings. */
static void txt(struct pkt *p, size_t inst, const char *const *strs,
		size_t count)
{
	put16(p, 0xC000 | (unsigned)inst);
	size_t rd = rr(p, 16, 4500);
	for (size_t i = 0; i < count; i++) {
		size_t n = strlen(strs[i]);
		put8(p, (unsigned)n);
		put_bytes(p, strs[i], n);
	}
	end_rdata(p, rd);
}

/* A whole, well-formed reply: PTR + TXT for one phone. */
static void phone_reply(struct pkt *p, const char *name, const char *id,
			const char *kind)
{
	char idkv[64], kindkv[32];
	snprintf(idkv, sizeof(idkv), "id=%s", id);
	snprintf(kindkv, sizeof(kindkv), "kind=%s", kind);
	const char *strs[] = {idkv, "proto=1", kindkv};
	size_t svc = 0;
	begin(p);
	size_t inst = ptr(p, &svc, name, 4500);
	txt(p, inst, strs, 3);
	finish(p);
}

static int parse(const struct pkt *p, const char *host,
		 struct mdns_result *r, int count, int max)
{
	/* Parse from an exact-size heap copy so ASan sees any overread. */
	uint8_t *copy = malloc(p->n ? p->n : 1);
	memcpy(copy, p->b, p->n);
	int n = mdns_parse_response(copy, p->n, g_service, g_service_len,
				    host, r, count, max);
	free(copy);
	return n;
}

/* --- tests ------------------------------------------------------------ */

static void test_valid(void)
{
	struct pkt p;
	struct mdns_result r[4];

	phone_reply(&p, "Emma's iPhone", ID_A, "camera");
	CHECK(parse(&p, "192.168.1.47", r, 0, 4) == 1);
	CHECK_STR(r[0].name, "Emma's iPhone");
	CHECK_STR(r[0].host, "192.168.1.47");
	CHECK_STR(r[0].device_id, ID_A);
	CHECK(r[0].protocol == 1 && r[0].kind == MDNS_KIND_CAMERA);

	/* Lower-case id in TXT is canonicalized; keys match any case. */
	const char *strs[] = {"ID=b1092868-1234-5678-abcd-0123456789ab",
			      "Kind=screen", "PROTO=2"};
	size_t svc = 0;
	begin(&p);
	size_t inst = ptr(&p, &svc, "Mirror", 120);
	txt(&p, inst, strs, 3);
	finish(&p);
	CHECK(parse(&p, "10.0.0.2", r, 0, 4) == 1);
	CHECK_STR(r[0].device_id, ID_A);
	CHECK(r[0].kind == MDNS_KIND_SCREEN && r[0].protocol == 2);
}

static void test_missing(void)
{
	struct pkt p;
	struct mdns_result r[4];
	size_t svc = 0;

	/* An app older than device identity: PTR, no TXT. Still listed
	 * (the Phone dropdown needs it), with no id. */
	begin(&p);
	ptr(&p, &svc, "Old iPhone", 4500);
	finish(&p);
	CHECK(parse(&p, "192.168.1.9", r, 0, 4) == 1);
	CHECK_STR(r[0].name, "Old iPhone");
	CHECK(!r[0].device_id[0] && !r[0].protocol &&
	      r[0].kind == MDNS_KIND_UNKNOWN);

	/* TXT without an id key; an empty TXT (one zero-length string);
	 * an id key with no value; boolean-style "id". */
	static const char *const variants[][2] = {
		{"proto=1", "kind=camera"},
		{"", ""},
		{"id=", "kind=camera"},
		{"id", "kind=camera"},
		{"=" ID_A, "kind=camera"}, /* missing key: ignored */
	};
	for (size_t v = 0; v < sizeof(variants) / sizeof(variants[0]); v++) {
		svc = 0;
		begin(&p);
		size_t inst = ptr(&p, &svc, "Phone", 4500);
		txt(&p, inst, variants[v], 2);
		finish(&p);
		CHECK(parse(&p, "192.168.1.9", r, 0, 4) == 1);
		CHECK(!r[0].device_id[0]);
	}

	/* A TXT for a *different* owner doesn't attach to our instance. */
	const char *other[] = {"id=" ID_B};
	svc = 0;
	begin(&p);
	ptr(&p, &svc, "Phone", 4500);
	size_t stranger = put_name(&p, "Printer._ipp._tcp.local");
	size_t rd = rr(&p, 16, 4500);
	put8(&p, (unsigned)strlen(other[0]));
	put_bytes(&p, other[0], strlen(other[0]));
	end_rdata(&p, rd);
	(void)stranger;
	finish(&p);
	CHECK(parse(&p, "192.168.1.9", r, 0, 4) == 1);
	CHECK(!r[0].device_id[0]);

	/* No PTR for our service at all: nothing added. */
	begin(&p);
	put_name(&p, "_other._tcp.local");
	rd = rr(&p, 12, 4500);
	put_name(&p, "x._other._tcp.local");
	end_rdata(&p, rd);
	finish(&p);
	CHECK(parse(&p, "192.168.1.9", r, 0, 4) == 0);
}

static void test_txt_rules(void)
{
	struct mdns_result r;

	/* Duplicate id: dropped entirely, even with the same value. */
	const uint8_t dup[] = "\x27id=B1092868-1234-5678-ABCD-0123456789AB"
			      "\x27id=0F5E2D4C-9A8B-4C7D-8E6F-102030405060";
	memset(&r, 0, sizeof(r));
	CHECK(mdns_parse_txt(dup, sizeof(dup) - 1, &r) && !r.device_id[0]);
	const uint8_t same[] = "\x27id=B1092868-1234-5678-ABCD-0123456789AB"
			       "\x27ID=B1092868-1234-5678-ABCD-0123456789AB";
	CHECK(mdns_parse_txt(same, sizeof(same) - 1, &r) && !r.device_id[0]);

	/* Other duplicate keys: first wins (RFC 6763 §6.4). */
	const uint8_t kinds[] = "\x0bkind=screen\x0bkind=camera\x07proto=1"
				"\x07proto=9";
	CHECK(mdns_parse_txt(kinds, sizeof(kinds) - 1, &r));
	CHECK(r.kind == MDNS_KIND_SCREEN && r.protocol == 1);

	/* Length byte running past the rdata: malformed, nothing filled. */
	const uint8_t overrun[] = "\x30id=B1092868-1234-5678-ABCD-0123456789AB";
	memset(&r, 0, sizeof(r));
	CHECK(!mdns_parse_txt(overrun, sizeof(overrun) - 1, &r));
	CHECK(!r.device_id[0]);
	const uint8_t lone[] = {5};
	CHECK(!mdns_parse_txt(lone, 1, &r));
	CHECK(mdns_parse_txt(lone, 0, &r)); /* empty rdata: nothing */

	/* Oversized / wrong values. */
	const uint8_t longid[] =
		"\x29id=B1092868-1234-5678-ABCD-0123456789ABCD";
	CHECK(mdns_parse_txt(longid, sizeof(longid) - 1, &r));
	CHECK(!r.device_id[0]);
	uint8_t huge[256];
	huge[0] = 255;
	memcpy(huge + 1, "id=", 3);
	memset(huge + 4, 'A', 252);
	CHECK(mdns_parse_txt(huge, sizeof(huge), &r) && !r.device_id[0]);
	const uint8_t nul[] = "\x27id=B1092868-1234-5678-ABCD-01234\0" "6789AB";
	CHECK(mdns_parse_txt(nul, sizeof(nul) - 1, &r) && !r.device_id[0]);
	const uint8_t proto[] = "\x09proto=256\x08kind=Cam";
	CHECK(mdns_parse_txt(proto, sizeof(proto) - 1, &r));
	CHECK(r.protocol == 0 && r.kind == MDNS_KIND_UNKNOWN);
	const uint8_t proto2[] = "\x08proto=+1\x0bkind=screen";
	CHECK(mdns_parse_txt(proto2, sizeof(proto2) - 1, &r));
	CHECK(r.protocol == 0 && r.kind == MDNS_KIND_SCREEN);
	/* Non-printable key bytes: the entry is ignored. */
	const uint8_t badkey[] = "\x29\x01id\x7f=B1092868-1234-5678-ABCD-"
				 "0123456789AB";
	CHECK(mdns_parse_txt(badkey, sizeof(badkey) - 1, &r) &&
	      !r.device_id[0]);
}

static void test_compression(void)
{
	struct pkt p;
	struct mdns_result r[4];

	/* The usual shape: PTR target and TXT owner both compressed
	 * against earlier names — covered by phone_reply. A TXT whose
	 * owner is spelled out uncompressed, in another case, matches. */
	const char *strs[] = {"id=" ID_B};
	size_t svc = 0;
	begin(&p);
	ptr(&p, &svc, "Stage Left", 4500);
	put_name(&p, "STAGE LEFT._LensLink._TCP.local");
	size_t rd = rr(&p, 16, 4500);
	put8(&p, (unsigned)strlen(strs[0]));
	put_bytes(&p, strs[0], strlen(strs[0]));
	end_rdata(&p, rd);
	finish(&p);
	CHECK(parse(&p, "10.1.1.1", r, 0, 4) == 1);
	CHECK_STR(r[0].device_id, ID_B);

	/* A label containing a dot must not alias two labels. */
	svc = 0;
	begin(&p);
	ptr(&p, &svc, "Stage.Left", 4500);
	put_name(&p, "Stage.Left._lenslink._tcp.local"); /* 2 labels */
	rd = rr(&p, 16, 4500);
	put8(&p, (unsigned)strlen(strs[0]));
	put_bytes(&p, strs[0], strlen(strs[0]));
	end_rdata(&p, rd);
	finish(&p);
	CHECK(parse(&p, "10.1.1.1", r, 0, 4) == 1);
	CHECK_STR(r[0].name, "Stage.Left");
	CHECK(!r[0].device_id[0]);

	/* Compression loops: a name pointing at itself, and two names
	 * pointing at each other. */
	begin(&p);
	size_t self = p.n;
	put16(&p, 0xC000 | (unsigned)self);
	rr(&p, 12, 4500);
	finish(&p);
	CHECK(parse(&p, "10.1.1.1", r, 0, 4) == 0);

	begin(&p);
	put_name(&p, SERVICE);
	size_t rd2 = rr(&p, 12, 4500);
	size_t a = p.n;
	put16(&p, 0xC000 | (unsigned)(a + 2));
	put16(&p, 0xC000 | (unsigned)a);
	end_rdata(&p, rd2);
	finish(&p);
	CHECK(parse(&p, "10.1.1.1", r, 0, 4) == 0);

	/* A pointer past the end, and a reserved label type (0x40). */
	begin(&p);
	put16(&p, 0xC000 | 0x3FFF);
	rr(&p, 12, 4500);
	finish(&p);
	CHECK(parse(&p, "10.1.1.1", r, 0, 4) == 0);
	begin(&p);
	put8(&p, 0x41);
	put8(&p, 'x');
	put8(&p, 0);
	rr(&p, 12, 4500);
	finish(&p);
	CHECK(parse(&p, "10.1.1.1", r, 0, 4) == 0);

	/* A name longer than 255 bytes via a pointer chain of long labels
	 * is rejected, not overflowed. */
	begin(&p);
	size_t prev = 0;
	char label[63];
	memset(label, 'a', sizeof(label));
	for (int i = 0; i < 6; i++) {
		size_t at = p.n;
		put8(&p, 63);
		put_bytes(&p, label, 63);
		if (prev)
			put16(&p, 0xC000 | (unsigned)prev);
		else
			put8(&p, 0);
		prev = at;
	}
	put16(&p, 0xC000 | (unsigned)prev);
	rr(&p, 12, 4500);
	finish(&p);
	CHECK(parse(&p, "10.1.1.1", r, 0, 4) == 0);
}

static void test_hostile(void)
{
	struct pkt p;
	struct mdns_result r[4];

	/* Not a response (QR clear): ignored. */
	phone_reply(&p, "Phone", ID_A, "camera");
	p.b[2] = 0;
	CHECK(parse(&p, "10.1.1.1", r, 0, 4) == 0);

	/* Every truncation of a valid reply parses without overreading;
	 * a cut before the PTR's rdata ends yields nothing, and no cut
	 * ever yields an id that isn't the real one. */
	phone_reply(&p, "Phone", ID_A, "camera");
	struct pkt cut = p;
	for (size_t n = 0; n < p.n; n++) {
		cut.n = n;
		int c = parse(&cut, "10.1.1.1", r, 0, 4);
		CHECK(c == 0 || c == 1);
		if (c == 1 && r[0].device_id[0])
			CHECK_STR(r[0].device_id, ID_A);
	}
	CHECK(parse(&p, "10.1.1.1", r, 0, 4) == 1);

	/* Record counts far beyond the packet: bounded, no overread. */
	phone_reply(&p, "Phone", ID_A, "camera");
	p.b[4] = p.b[5] = p.b[6] = p.b[7] = p.b[8] = p.b[9] = 0xFF;
	CHECK(parse(&p, "10.1.1.1", r, 0, 4) == 0); /* bogus questions */
	phone_reply(&p, "Phone", ID_A, "camera");
	p.b[6] = p.b[7] = 0xFF;
	CHECK(parse(&p, "10.1.1.1", r, 0, 4) == 1); /* intact prefix */
	CHECK_STR(r[0].device_id, ID_A);

	/* rdlength past the packet. */
	phone_reply(&p, "Phone", ID_A, "camera");
	size_t rd = 12 + 22 + 8; /* after the service name + fixed fields */
	p.b[rd] = 0xFF;
	p.b[rd + 1] = 0xFF;
	CHECK(parse(&p, "10.1.1.1", r, 0, 4) == 0);

	/* Goodbye (TTL 0): the service is leaving; not listed. */
	size_t svc = 0;
	begin(&p);
	ptr(&p, &svc, "Leaving", 0);
	finish(&p);
	CHECK(parse(&p, "10.1.1.1", r, 0, 4) == 0);

	/* A display name with a NUL, control bytes and invalid UTF-8 is
	 * made printable, never cut short at the NUL. */
	svc = 0;
	begin(&p);
	put_name(&p, SERVICE);
	svc = 12;
	size_t rdp = rr(&p, 12, 4500);
	const char evil[] = "Emma\0\x07\xFF\xC3\x97";
	put_label_ptr(&p, evil, sizeof(evil) - 1, svc);
	end_rdata(&p, rdp);
	finish(&p);
	CHECK(parse(&p, "10.1.1.1", r, 0, 4) == 1);
	CHECK_STR(r[0].name, "Emma\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD"
			     "\xC3\x97");

	/* Bad from_host arguments. */
	phone_reply(&p, "Phone", ID_A, "camera");
	CHECK(parse(&p, "", r, 0, 4) == 0);
	char longhost[80];
	memset(longhost, '1', sizeof(longhost) - 1);
	longhost[sizeof(longhost) - 1] = 0;
	CHECK(parse(&p, longhost, r, 0, 4) == 0);
}

static void test_multi_device(void)
{
	struct pkt p;
	struct mdns_result r[4];
	int n = 0;

	/* Two phones with the same display name, different ids. */
	phone_reply(&p, "iPhone", ID_A, "camera");
	n = parse(&p, "192.168.1.10", r, n, 4);
	phone_reply(&p, "iPhone", ID_B, "camera");
	n = parse(&p, "192.168.1.11", r, n, 4);
	CHECK(n == 2);
	const struct mdns_result *a = mdns_find_device(r, n, ID_A);
	const struct mdns_result *b = mdns_find_device(r, n, ID_B);
	CHECK(a && b && strcmp(a->host, "192.168.1.10") == 0 &&
	      strcmp(b->host, "192.168.1.11") == 0);
	CHECK(!mdns_find_device(r, n, "") && !mdns_find_device(r, n, NULL));
	CHECK(!mdns_find_device(r, n, "0F5E2D4C-9A8B-4C7D-8E6F-000000000000"));

	/* A repeated reply from one address doesn't duplicate it. */
	n = parse(&p, "192.168.1.11", r, n, 4);
	CHECK(n == 2);

	/* A later packet from the same address fills a missing TXT, but
	 * never replaces an id it already had. */
	struct mdns_result r2[4];
	size_t svc = 0;
	begin(&p);
	ptr(&p, &svc, "Phone", 4500);
	finish(&p);
	int m = parse(&p, "10.0.0.5", r2, 0, 4);
	phone_reply(&p, "Phone", ID_A, "camera");
	m = parse(&p, "10.0.0.5", r2, m, 4);
	CHECK(m == 1);
	CHECK_STR(r2[0].device_id, ID_A);
	phone_reply(&p, "Phone", ID_B, "camera");
	m = parse(&p, "10.0.0.5", r2, m, 4);
	CHECK(m == 1);
	CHECK_STR(r2[0].device_id, ID_A);

	/* max_results is respected. */
	struct mdns_result one[1];
	phone_reply(&p, "P", ID_A, "camera");
	int k = parse(&p, "10.0.0.1", one, 0, 1);
	phone_reply(&p, "Q", ID_B, "camera");
	k = parse(&p, "10.0.0.2", one, k, 1);
	CHECK(k == 1);
	CHECK_STR(one[0].device_id, ID_A);
}

static void test_rename_and_move(void)
{
	struct pkt p;
	struct mdns_result r[4];

	/* Renamed phone, same id: found by id, new name reported. */
	phone_reply(&p, "Gibson's iPhone", ID_A, "camera");
	int n = parse(&p, "192.168.1.47", r, 0, 4);
	const struct mdns_result *f = mdns_find_device(r, n, ID_A);
	CHECK(f && strcmp(f->name, "Gibson's iPhone") == 0);

	phone_reply(&p, "Stage Left Cam", ID_A, "camera");
	n = parse(&p, "192.168.1.47", r, 0, 4);
	f = mdns_find_device(r, n, ID_A);
	CHECK(f && strcmp(f->name, "Stage Left Cam") == 0);

	/* Same id at a new address (DHCP moved it): the new address. */
	n = parse(&p, "192.168.1.88", r, 0, 4);
	f = mdns_find_device(r, n, ID_A);
	CHECK(f && strcmp(f->host, "192.168.1.88") == 0);

	/* And the old address now answering as someone else resolves
	 * that someone else, not us. */
	phone_reply(&p, "Other", ID_B, "camera");
	n = parse(&p, "192.168.1.47", r, 0, 4);
	CHECK(!mdns_find_device(r, n, ID_A));
}

int main(void)
{
	int n = mdns_encode_name(SERVICE, g_service, sizeof(g_service));
	CHECK(n > 0);
	g_service_len = (size_t)n;
	CHECK(mdns_encode_name("a..b", g_service + 32, 32) < 0);

	test_valid();
	test_missing();
	test_txt_rules();
	test_compression();
	test_hostile();
	test_multi_device();
	test_rename_and_move();
	TEST_MAIN_END();
}
