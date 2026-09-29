/*
 * A small, strict, allocation-free JSON pull reader for the few JSON
 * payloads the plugin receives from the phone (STATE today).
 *
 * Why not obs_data_create_from_json: libobs's importer keeps only arrays
 * of *objects* and silently drops arrays of strings and numbers — exactly
 * what STATE uses for its capability lists (lenses, resolutions,
 * frameRates, codecs). Linking jansson directly isn't an option either:
 * OBS links it privately and doesn't ship it to plugins on every
 * platform.
 *
 * The reader walks a bounded buffer (no NUL terminator needed), never
 * reads past `len`, limits nesting depth, and decodes strings into
 * caller-owned buffers, truncating only on a UTF-8 code point boundary.
 * Any syntax error latches `error`; every later call then fails, so a
 * caller can check once at the end. Pure C with no libobs dependency, so
 * it can be unit-tested standalone.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define JSON_READER_MAX_DEPTH 16

enum json_type {
	JSON_TYPE_INVALID, /* end of input or a syntax error */
	JSON_TYPE_OBJECT,
	JSON_TYPE_ARRAY,
	JSON_TYPE_STRING,
	JSON_TYPE_NUMBER,
	JSON_TYPE_BOOL,
	JSON_TYPE_NULL,
};

struct json_reader {
	const char *p;
	const char *end;
	int depth;
	/* Bit d set: an item was already read at nesting depth d, so the
	 * next one there must be preceded by a comma. */
	unsigned int has_item;
	bool error;
};

void json_reader_init(struct json_reader *r, const char *json, size_t len);

/* The type of the next value, without consuming it. */
enum json_type json_peek(struct json_reader *r);

/* Objects: json_object_begin consumes '{'; then call json_object_next
 * until it returns false (the closing '}' consumed, or an error). Each
 * true return has read one key and its ':' — the caller must then consume
 * exactly one value (read it or json_skip it). `key_truncated` reports a
 * key that did not fit, which callers should treat as unknown. */
bool json_object_begin(struct json_reader *r);
bool json_object_next(struct json_reader *r, char *key, size_t key_size,
		      bool *key_truncated);

/* Arrays: same pattern — json_array_next returns true while another
 * element follows (the caller consumes it), false at the closing ']'. */
bool json_array_begin(struct json_reader *r);
bool json_array_next(struct json_reader *r);

/* Scalar reads fail (without latching an error) when the next value has
 * a different type, so a caller can fall back to json_skip. */
bool json_read_string(struct json_reader *r, char *out, size_t out_size,
		      bool *truncated);
bool json_read_number(struct json_reader *r, double *out);
bool json_read_bool(struct json_reader *r, bool *out);

/* Consumes one value of any type, nested ones included. */
bool json_skip(struct json_reader *r);

/* True when the whole input was consumed (trailing whitespace allowed)
 * and no error occurred. */
bool json_reader_done(struct json_reader *r);

#ifdef __cplusplus
}
#endif
