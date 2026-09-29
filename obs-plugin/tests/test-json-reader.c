#include "json-reader.h"
#include "test.h"

static struct json_reader reader(const char *json)
{
	struct json_reader r;
	json_reader_init(&r, json, strlen(json));
	return r;
}

/* Consumes one whole value; true if it was valid JSON with nothing after. */
static bool valid(const char *json)
{
	struct json_reader r = reader(json);
	return json_skip(&r) && json_reader_done(&r);
}

static void test_scalars(void)
{
	struct json_reader r;
	double d;
	bool b;

	r = reader("42");
	CHECK(json_read_number(&r, &d) && d == 42.0);
	r = reader("-0.5");
	CHECK(json_read_number(&r, &d));
	CHECK_NEAR(d, -0.5);
	r = reader("1.25e2");
	CHECK(json_read_number(&r, &d));
	CHECK_NEAR(d, 125.0);
	r = reader("2.5E-3");
	CHECK(json_read_number(&r, &d));
	CHECK_NEAR(d, 0.0025);
	r = reader("0.000123");
	CHECK(json_read_number(&r, &d));
	CHECK_NEAR(d, 0.000123);
	r = reader("0.016666666666666666");
	CHECK(json_read_number(&r, &d));
	CHECK_NEAR(d, 1.0 / 60.0);
	r = reader("1e999");
	CHECK(json_read_number(&r, &d) && d > 1e300); /* inf, not a crash */
	r = reader("1e-999");
	CHECK(json_read_number(&r, &d) && d == 0.0);

	r = reader(" true ");
	CHECK(json_read_bool(&r, &b) && b && json_reader_done(&r));
	r = reader("false");
	CHECK(json_read_bool(&r, &b) && !b);

	/* Type mismatch fails without latching an error. */
	r = reader("\"x\"");
	CHECK(!json_read_number(&r, &d) && !r.error);
	CHECK(json_skip(&r) && json_reader_done(&r));
}

static void test_invalid_numbers(void)
{
	CHECK(!valid("01"));
	CHECK(!valid("1."));
	CHECK(!valid(".5"));
	CHECK(!valid("-"));
	CHECK(!valid("1e"));
	CHECK(!valid("+1"));
	CHECK(!valid("0x10"));
	CHECK(!valid("NaN"));
	CHECK(!valid("Infinity"));
}

static void test_strings(void)
{
	struct json_reader r;
	char buf[32];
	bool trunc;

	r = reader("\"a\\\"b\\\\c\\/d\\n\"");
	CHECK(json_read_string(&r, buf, sizeof(buf), &trunc) && !trunc);
	CHECK_STR(buf, "a\"b\\c/d\n");

	/* \u escapes, including a surrogate pair (U+1F600). */
	r = reader("\"\\u00d7\\u20ac\\ud83d\\ude00\"");
	CHECK(json_read_string(&r, buf, sizeof(buf), &trunc));
	CHECK_STR(buf, "\xC3\x97\xE2\x82\xAC\xF0\x9F\x98\x80");

	/* Lone surrogates become U+FFFD. */
	r = reader("\"\\ud83dx\\ude00\"");
	CHECK(json_read_string(&r, buf, sizeof(buf), &trunc));
	CHECK_STR(buf, "\xEF\xBF\xBDx\xEF\xBF\xBD");

	/* Raw UTF-8 passes through; malformed bytes become U+FFFD. */
	r = reader("\"0.5\xC3\x97 \xFF\"");
	CHECK(json_read_string(&r, buf, sizeof(buf), &trunc));
	CHECK_STR(buf, "0.5\xC3\x97 \xEF\xBF\xBD");

	/* Truncation lands on a code point boundary: "ab×" in 4 bytes
	 * (3 + NUL) keeps "ab" rather than half of the ×. */
	char small[4];
	r = reader("\"ab\xC3\x97z\"");
	CHECK(json_read_string(&r, small, sizeof(small), &trunc) && trunc);
	CHECK_STR(small, "ab");
	CHECK(json_reader_done(&r)); /* still consumed the whole string */

	CHECK(!valid("\"unterminated"));
	CHECK(!valid("\"bad \\x escape\""));
	CHECK(!valid("\"\\u12\""));
	CHECK(!valid("\"raw\ncontrol\""));
}

static void test_structures(void)
{
	CHECK(valid("{}"));
	CHECK(valid("[]"));
	CHECK(valid("{\"a\":[1,2,{\"b\":null}],\"c\":\"d\"}"));
	CHECK(valid(" { \"a\" : 1 , \"b\" : [ ] } "));

	CHECK(!valid("{\"a\":1,}"));
	CHECK(!valid("[1,]"));
	CHECK(!valid("[,1]"));
	CHECK(!valid("{,}"));
	CHECK(!valid("{\"a\" 1}"));
	CHECK(!valid("{\"a\":}"));
	CHECK(!valid("{\"a\":1"));
	CHECK(!valid("{\"a\":1}}"));
	CHECK(!valid("{\"a\":1} x"));
	CHECK(!valid("[1 2]"));
	CHECK(!valid("{1:2}"));
	CHECK(!valid(""));
	CHECK(!valid("   "));
	CHECK(!valid("nul"));
	CHECK(!valid("tru"));

	/* Nesting deeper than the limit is an error, not a stack overflow. */
	char deep[256];
	size_t n = 0;
	for (int i = 0; i < 100; i++)
		deep[n++] = '[';
	for (int i = 0; i < 100; i++)
		deep[n++] = ']';
	deep[n] = 0;
	CHECK(!valid(deep));

	/* Within the limit is fine. */
	CHECK(valid("[[[[[[[[1]]]]]]]]"));
}

static void test_object_walk(void)
{
	struct json_reader r = reader(
		"{\"name\":\"Stage Left\",\"n\":3,\"list\":[\"x\",\"y\"]}");
	char key[8], val[32];
	bool trunc = false;
	int keys = 0;

	CHECK(json_object_begin(&r));
	while (json_object_next(&r, key, sizeof(key), &trunc)) {
		keys++;
		if (strcmp(key, "name") == 0) {
			CHECK(json_read_string(&r, val, sizeof(val), NULL));
			CHECK_STR(val, "Stage Left");
		} else if (strcmp(key, "list") == 0) {
			int items = 0;
			CHECK(json_array_begin(&r));
			while (json_array_next(&r)) {
				CHECK(json_read_string(&r, val, sizeof(val),
						       NULL));
				items++;
			}
			CHECK(items == 2);
		} else {
			CHECK(json_skip(&r));
		}
	}
	CHECK(keys == 3);
	CHECK(json_reader_done(&r));

	/* A key longer than the buffer reports truncation. */
	r = reader("{\"averyveryverylongkey\":1}");
	CHECK(json_object_begin(&r));
	CHECK(json_object_next(&r, key, sizeof(key), &trunc) && trunc);
	CHECK(json_skip(&r));
	CHECK(!json_object_next(&r, key, sizeof(key), &trunc));
	CHECK(json_reader_done(&r));
}

static void test_bounds(void)
{
	/* The reader must stop at `len`, not at a NUL: the payload is a
	 * packet buffer. Everything after the 7th byte is out of bounds. */
	const char buf[] = "{\"a\":1}GARBAGE";
	struct json_reader r;
	json_reader_init(&r, buf, 7);
	CHECK(json_skip(&r) && json_reader_done(&r));

	/* A NUL inside the bounds is invalid JSON, not a terminator. */
	const char nul[] = {'{', '}', 0, ' '};
	json_reader_init(&r, nul, sizeof(nul));
	CHECK(json_skip(&r) && !json_reader_done(&r));

	json_reader_init(&r, NULL, 0);
	CHECK(!json_skip(&r));

	/* Every prefix of a valid document is rejected cleanly. */
	const char doc[] = "{\"a\":[1,\"\\u00d7\",true,null],\"b\":{\"c\":-2.5e1}}";
	for (size_t len = 0; len + 1 < sizeof(doc); len++) {
		json_reader_init(&r, doc, len);
		CHECK(!(json_skip(&r) && json_reader_done(&r)));
	}
	json_reader_init(&r, doc, sizeof(doc) - 1);
	CHECK(json_skip(&r) && json_reader_done(&r));
}

int main(void)
{
	test_scalars();
	test_invalid_numbers();
	test_strings();
	test_structures();
	test_object_walk();
	test_bounds();
	TEST_MAIN_END();
}
