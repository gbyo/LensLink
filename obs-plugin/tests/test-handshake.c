/* Device ids, HELLO / VIDEO_CONFIG parsing, and the identity verdict. */

#include "device-identity.h"
#include "handshake.h"
#include "test.h"

#include <stdlib.h>

#define ID_A "B1092868-1234-5678-ABCD-0123456789AB"
#define ID_B "0F5E2D4C-9A8B-4C7D-8E6F-102030405060"

static bool id_ok(const char *s, size_t len, const char *want)
{
	char out[LENSLINK_DEVICE_ID_SIZE];
	memset(out, 'x', sizeof(out));
	bool ok = lenslink_device_id_parse(s, len, out);
	if (!ok)
		return out[0] == 0 && want == NULL;
	return want && strcmp(out, want) == 0;
}

#define ID_OK(lit, want) id_ok((lit), sizeof(lit) - 1, (want))
#define ID_BAD(lit) id_ok((lit), sizeof(lit) - 1, NULL)

static void test_device_id(void)
{
	/* Canonical, and lower/mixed case canonicalized to upper. */
	CHECK(ID_OK(ID_A, ID_A));
	CHECK(ID_OK("b1092868-1234-5678-abcd-0123456789ab", ID_A));
	CHECK(ID_OK("B1092868-1234-5678-aBcD-0123456789Ab", ID_A));
	CHECK(lenslink_device_id_valid(ID_B));

	/* Missing / empty. */
	CHECK(!lenslink_device_id_valid(NULL));
	CHECK(!lenslink_device_id_valid(""));
	CHECK(id_ok(NULL, 36, NULL));
	CHECK(ID_BAD(""));

	/* Wrong length: truncated, too long, trailing/leading junk. */
	CHECK(ID_BAD("B1092868-1234-5678-ABCD-0123456789A"));
	CHECK(ID_BAD("B1092868-1234-5678-ABCD-0123456789ABC"));
	CHECK(ID_BAD(" B1092868-1234-5678-ABCD-0123456789AB"));
	CHECK(ID_BAD("B1092868-1234-5678-ABCD-0123456789AB\n"));
	CHECK(ID_BAD("{B1092868-1234-5678-ABCD-0123456789AB}"));
	CHECK(ID_BAD("urn:uuid:B1092868-1234-5678-ABCD-0123456789AB"));

	/* Wrong separators / separator count / positions. */
	CHECK(ID_BAD("B10928681234-5678-ABCD-0123456789AB-"));
	CHECK(ID_BAD("B1092868_1234_5678_ABCD_0123456789AB"));
	CHECK(ID_BAD("B1092868-1234-5678-ABCD-01234567-9AB"));
	CHECK(ID_BAD("B1092868-12345678-ABCD-0123456789AB0"));
	CHECK(ID_BAD("B1092868123456789ABCD0123456789ABCDE"));
	CHECK(ID_BAD("------------------------------------"));

	/* Non-hex. */
	CHECK(ID_BAD("G1092868-1234-5678-ABCD-0123456789AB"));
	CHECK(ID_BAD("B1092868-1234-5678-ABCD-0123456789AZ"));
	CHECK(ID_BAD("B1092868-1234-5678-ABCD-01234567 9AB"));

	/* Embedded NUL: the length says 36 but a C-string view would stop
	 * early — must be rejected, never read as some other id. */
	CHECK(ID_BAD("B1092868-1234-5678-ABCD-01234\0" "6789AB"));
	CHECK(!lenslink_device_id_valid("B1092868-1234-5678-ABCD-01234\0"));

	/* Unicode tricks: full-width digits, a Cyrillic "В" for B, a
	 * non-breaking hyphen — all non-ASCII bytes, all rejected.
	 * Each is exactly 36 bytes, so the length check can't be what
	 * rejects it. */
#define ID_BAD36(lit) (sizeof(lit) - 1 == 36 && ID_BAD(lit))
	CHECK(ID_BAD36("\xD0\x92" "092868-1234-5678-ABCD-0123456789AB"));
	CHECK(ID_BAD36("\xEF\xBC\x91" "92868-1234-5678-ABCD-0123456789AB"));
	CHECK(ID_BAD36("B1092868\xE2\x80\x91" "34-5678-ABCD-0123456789AB"));
	CHECK(ID_BAD36("B1092868-1234-5678-ABCD-0123456789\xC3\x9F"));

	/* Nil and max UUIDs are placeholders. */
	CHECK(ID_BAD("00000000-0000-0000-0000-000000000000"));
	CHECK(ID_BAD("FFFFFFFF-FFFF-FFFF-FFFF-FFFFFFFFFFFF"));
	CHECK(ID_BAD("ffffffff-ffff-ffff-ffff-ffffffffffff"));
}

static void test_verify(void)
{
	/* The connection-identity decision table. */
	CHECK(lenslink_identity_verify(ID_A, ID_A) ==
	      LENSLINK_IDENTITY_VERIFIED);
	CHECK(lenslink_identity_verify(ID_A, ID_B) ==
	      LENSLINK_IDENTITY_MISMATCH);
	CHECK(lenslink_identity_verify(ID_A, "") ==
	      LENSLINK_IDENTITY_UNIDENTIFIED);
	CHECK(lenslink_identity_verify(ID_A, NULL) ==
	      LENSLINK_IDENTITY_UNIDENTIFIED);
	CHECK(lenslink_identity_verify("", "") == LENSLINK_IDENTITY_UNBOUND);
	CHECK(lenslink_identity_verify(NULL, ID_B) ==
	      LENSLINK_IDENTITY_UNBOUND);

	CHECK(lenslink_identity_accepted(LENSLINK_IDENTITY_UNBOUND));
	CHECK(lenslink_identity_accepted(LENSLINK_IDENTITY_VERIFIED));
	CHECK(!lenslink_identity_accepted(LENSLINK_IDENTITY_MISMATCH));
	CHECK(!lenslink_identity_accepted(LENSLINK_IDENTITY_UNIDENTIFIED));
	CHECK(!lenslink_identity_accepted(LENSLINK_IDENTITY_SEARCHING));
	CHECK_STR(lenslink_identity_state_name(LENSLINK_IDENTITY_MISMATCH),
		  "mismatch");
}

static bool hello(const char *json, struct lenslink_hello *h)
{
	return lenslink_hello_parse(json, strlen(json), h);
}

static bool all_absent(const struct lenslink_hello *h)
{
	return !h->valid && !h->name[0] && !h->app[0] && !h->protocol &&
	       !h->is_screen && !h->standby && !h->device_id[0];
}

static void test_hello_basic(void)
{
	struct lenslink_hello h;

	/* An app older than device identity. */
	CHECK(hello("{\"name\":\"Emma's iPhone\",\"app\":\"LensLink\","
		    "\"protocol\":1,\"kind\":\"camera\"}",
		    &h));
	CHECK_STR(h.name, "Emma's iPhone");
	CHECK_STR(h.app, "LensLink");
	CHECK(h.protocol == 1 && !h.is_screen && !h.standby);
	CHECK(!h.device_id[0] && !h.device_id_invalid);

	/* A current app, any key order, lower-case id canonicalized. */
	CHECK(hello("{\"deviceID\":\"b1092868-1234-5678-abcd-0123456789ab\","
		    "\"kind\":\"camera\",\"name\":\"Gibson\\u2019s iPhone\","
		    "\"app\":\"LensLink\",\"protocol\":1}",
		    &h));
	CHECK_STR(h.device_id, ID_A);
	CHECK_STR(h.name, "Gibson\xE2\x80\x99s iPhone");

	/* Standby, screen, and the historical kind readings. */
	CHECK(hello("{\"kind\":\"camera\",\"standby\":true}", &h));
	CHECK(h.standby && !h.is_screen);
	CHECK(hello("{\"kind\":\"screen\"}", &h) && h.is_screen);
	CHECK(hello("{}", &h) && !h.is_screen); /* absent = camera */
	CHECK(hello("{\"kind\":\"toaster\"}", &h) && !h.is_screen);
	CHECK(hello("{\"standby\":false}", &h) && !h.standby);
	CHECK(hello("{\"standby\":\"true\"}", &h) && !h.standby);
	CHECK(hello("{\"standby\":1}", &h) && !h.standby);

	/* Unknown keys, including nested values, are skipped. */
	CHECK(hello("{\"future\":{\"a\":[1,{\"b\":null}]},\"name\":\"X\","
		    "\"deviceID\":\"" ID_B "\",\"more\":[\"s\"]}",
		    &h));
	CHECK_STR(h.name, "X");
	CHECK_STR(h.device_id, ID_B);
}

static void test_hello_field_isolation(void)
{
	struct lenslink_hello h;

	/* A malformed deviceID is absent — and only it. */
	CHECK(hello("{\"name\":\"N\",\"deviceID\":\"not-a-uuid\","
		    "\"kind\":\"screen\",\"standby\":true}",
		    &h));
	CHECK(!h.device_id[0] && h.device_id_invalid);
	CHECK_STR(h.name, "N");
	CHECK(h.is_screen && h.standby);

	static const char *const bad_ids[] = {
		"\"\"",
		"null",
		"42",
		"true",
		"[\"" ID_A "\"]",
		"{\"id\":\"" ID_A "\"}",
		"\"" ID_A " \"",
		"\"" ID_A "0\"",
		"\"B1092868-1234-5678-ABCD-0123456789A\"",
		"\"B1092868-1234-5678-ABCD-0123456789AB\\u0000\"",
		"\"B1092868-1234-5678-ABCD-01234\\u00006789AB\"",
		"\"\\u0042\\u0031092868-1234-5678-ABCD-0123456789AB\\u00ad\"",
		"\"B1092868\\u20111234-5678-ABCD-0123456789AB\"",
		"\"00000000-0000-0000-0000-000000000000\"",
	};
	for (size_t i = 0; i < sizeof(bad_ids) / sizeof(bad_ids[0]); i++) {
		char doc[256];
		snprintf(doc, sizeof(doc),
			 "{\"name\":\"N\",\"deviceID\":%s,\"kind\":\"screen\"}",
			 bad_ids[i]);
		CHECK(hello(doc, &h));
		CHECK(!h.device_id[0] && h.device_id_invalid);
		CHECK(h.is_screen && strcmp(h.name, "N") == 0);
	}

	/* Escaped A digits are fine: the *decoded* value is checked. */
	CHECK(hello("{\"deviceID\":\"\\u0042" "1092868-1234-5678-ABCD-"
		    "0123456789AB\"}",
		    &h));
	CHECK_STR(h.device_id, ID_A);

	/* Wrong types elsewhere don't disturb the id. */
	CHECK(hello("{\"name\":7,\"app\":[],\"protocol\":\"1\",\"kind\":{},"
		    "\"deviceID\":\"" ID_A "\"}",
		    &h));
	CHECK_STR(h.device_id, ID_A);
	CHECK(!h.name[0] && !h.app[0] && !h.protocol && !h.is_screen);

	/* A kind holding an escaped NUL can't smuggle "screen". */
	CHECK(hello("{\"kind\":\"screen\\u0000camera\"}", &h));
	CHECK(!h.is_screen);
	/* A name holding one is withheld, not cut short. */
	CHECK(hello("{\"name\":\"Stage\\u0000Left\"}", &h) && !h.name[0]);

	/* protocol: integers 1..255 only. */
	CHECK(hello("{\"protocol\":2}", &h) && h.protocol == 2);
	CHECK(hello("{\"protocol\":1.5}", &h) && h.protocol == 0);
	CHECK(hello("{\"protocol\":0}", &h) && h.protocol == 0);
	CHECK(hello("{\"protocol\":-1}", &h) && h.protocol == 0);
	CHECK(hello("{\"protocol\":1e999}", &h) && h.protocol == 0);
	CHECK(hello("{\"protocol\":256}", &h) && h.protocol == 0);

	/* A long name keeps a UTF-8-safe prefix (display only). */
	char doc[512];
	size_t n = (size_t)snprintf(doc, sizeof(doc), "{\"name\":\"");
	for (int i = 0; i < 100; i++)
		n += (size_t)snprintf(doc + n, sizeof(doc) - n, "\xC3\x97");
	snprintf(doc + n, sizeof(doc) - n, "\"}");
	CHECK(hello(doc, &h));
	CHECK(strlen(h.name) == 126); /* 63 whole ×, not 63.5 */

	/* A too-long app string is absent, not a prefix. */
	CHECK(hello("{\"app\":\"LensLinkLensLinkLensLinkLensLinkX\"}", &h));
	CHECK(!h.app[0]);
}

static void test_hello_invalid(void)
{
	struct lenslink_hello h;
	static const char *const bad[] = {
		"",
		"[]",
		"null",
		"\"hello\"",
		"{",
		"{\"name\":\"a\"",
		"{\"name\":\"a\",}",
		"{\"name\" \"a\"}",
		"{\"name\":\"a\"}{}",
		"{\"name\":\"a\"} x",
		"{name:\"a\"}",
		"{\"name\":\"unterminated}",
		"{\"name\":\"bad \\x escape\"}",
		"{\"name\":\"raw\ncontrol\"}",
		/* Duplicate known keys: ambiguous, whole packet rejected. */
		"{\"deviceID\":\"" ID_A "\",\"deviceID\":\"" ID_B "\"}",
		"{\"deviceID\":\"" ID_A "\",\"deviceID\":\"" ID_A "\"}",
		"{\"kind\":\"camera\",\"kind\":\"screen\"}",
		"{\"standby\":false,\"standby\":true}",
		"{\"name\":\"a\",\"name\":\"b\"}",
	};
	for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
		memset(&h, 0xAB, sizeof(h));
		CHECK(!hello(bad[i], &h));
		CHECK(all_absent(&h));
	}

	/* Duplicate *unknown* keys are harmless. */
	CHECK(hello("{\"x\":1,\"x\":2,\"deviceID\":\"" ID_A "\"}", &h));
	CHECK_STR(h.device_id, ID_A);

	/* Over the size cap. */
	char *big = malloc(LENSLINK_HANDSHAKE_MAX + 64);
	size_t n = (size_t)snprintf(big, 64, "{\"pad\":\"");
	while (n < LENSLINK_HANDSHAKE_MAX + 8)
		big[n++] = 'x';
	memcpy(big + n, "\"}", 3);
	CHECK(!lenslink_hello_parse(big, strlen(big), &h) && all_absent(&h));
	free(big);

	CHECK(!lenslink_hello_parse(NULL, 0, &h) && all_absent(&h));

	/* Every truncation of a real HELLO fails whole. */
	const char full[] = "{\"name\":\"Gibson\\u2019s iPhone\",\"deviceID\":"
			    "\"" ID_A "\",\"app\":\"LensLink\",\"protocol\":1,"
			    "\"kind\":\"camera\",\"standby\":true}";
	for (size_t len = 0; len + 1 < sizeof(full); len++) {
		char *copy = malloc(len ? len : 1);
		memcpy(copy, full, len);
		memset(&h, 0xAB, sizeof(h));
		CHECK(!lenslink_hello_parse(copy, len, &h));
		CHECK(all_absent(&h));
		free(copy);
	}
	CHECK(lenslink_hello_parse(full, sizeof(full) - 1, &h));
	CHECK_STR(h.device_id, ID_A);
	CHECK(h.standby && h.protocol == 1);

	/* The payload is a packet buffer: bytes past `len` are never read,
	 * and a NUL inside `len` is invalid, not a terminator. */
	const char trailing[] = "{\"kind\":\"screen\"}GARBAGE";
	CHECK(lenslink_hello_parse(trailing, 17, &h) && h.is_screen);
	const char nul[] = "{\"kind\":\"screen\"}\0";
	CHECK(!lenslink_hello_parse(nul, sizeof(nul) - 1, &h));
}

static void test_video_config(void)
{
	struct lenslink_video_config c;
#define VC(s) lenslink_video_config_parse((s), strlen(s), &c)

	CHECK(VC("{\"codec\":\"hevc\",\"width\":1920,\"height\":1080,"
		 "\"fps\":30,\"kind\":\"camera\",\"color\":\"hlg\"}"));
	CHECK(c.codec == LENSLINK_CODEC_HEVC && c.has_kind && !c.is_screen);
	CHECK(VC("{\"codec\":\"h264\",\"kind\":\"screen\"}"));
	CHECK(c.codec == LENSLINK_CODEC_H264 && c.has_kind && c.is_screen);
	/* Absent/empty/unusable kind leaves the connection's kind alone. */
	CHECK(VC("{\"codec\":\"hevc\"}") && !c.has_kind);
	CHECK(VC("{\"kind\":\"\"}") && !c.has_kind);
	CHECK(VC("{\"kind\":5}") && !c.has_kind);
	/* Unknown codec: H.264, as always. */
	CHECK(VC("{\"codec\":\"av1\"}") && c.codec == LENSLINK_CODEC_H264);
	CHECK(VC("{\"codec\":\"hevc\\u0000\"}") &&
	      c.codec == LENSLINK_CODEC_H264);
	/* Malformed / duplicated: defaults, invalid. */
	CHECK(!VC("{\"codec\":\"hevc\"") && c.codec == LENSLINK_CODEC_H264 &&
	      !c.has_kind);
	CHECK(!VC("{\"codec\":\"h264\",\"codec\":\"hevc\"}"));
	CHECK(!VC("{\"kind\":\"camera\",\"kind\":\"screen\"}") && !c.has_kind);
#undef VC
}

int main(void)
{
	test_device_id();
	test_verify();
	test_hello_basic();
	test_hello_field_isolation();
	test_hello_invalid();
	test_video_config();
	TEST_MAIN_END();
}
