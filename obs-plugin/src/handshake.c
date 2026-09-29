#include "handshake.h"
#include "json-reader.h"

#include <math.h>
#include <string.h>

/* Keys longer than this aren't ours; a truncated key is skipped. */
#define KEY_SIZE 32

/* Reads a string field. False (field absent) on a type mismatch, or when
 * the value didn't fit or held an escaped NUL and `need_whole` is set; a
 * type mismatch is skipped so the walk continues. */
static bool read_string_field(struct json_reader *r, char *out, size_t size,
			      bool need_whole)
{
	bool truncated = false;
	if (json_peek(r) != JSON_TYPE_STRING) {
		out[0] = 0;
		json_skip(r);
		return false;
	}
	if (!json_read_string(r, out, size, &truncated))
		return false;
	if (truncated && (need_whole || !out[0])) {
		out[0] = 0;
		return false;
	}
	return true;
}

/* A "kind" value: 1 = screen, 0 = camera, -1 = absent/unusable (not a
 * string, empty, too long). Unknown words read as camera, as they always
 * have. */
static int read_kind(struct json_reader *r)
{
	char kind[16];
	if (!read_string_field(r, kind, sizeof(kind), true) || !kind[0])
		return -1;
	return strcmp(kind, "screen") == 0 ? 1 : 0;
}

/* Bit per known key, to detect duplicates. */
enum {
	HELLO_NAME = 1 << 0,
	HELLO_APP = 1 << 1,
	HELLO_PROTOCOL = 1 << 2,
	HELLO_KIND = 1 << 3,
	HELLO_STANDBY = 1 << 4,
	HELLO_DEVICE_ID = 1 << 5,
};

static int hello_key(const char *key)
{
	if (strcmp(key, "name") == 0)
		return HELLO_NAME;
	if (strcmp(key, "app") == 0)
		return HELLO_APP;
	if (strcmp(key, "protocol") == 0)
		return HELLO_PROTOCOL;
	if (strcmp(key, "kind") == 0)
		return HELLO_KIND;
	if (strcmp(key, "standby") == 0)
		return HELLO_STANDBY;
	if (strcmp(key, "deviceID") == 0)
		return HELLO_DEVICE_ID;
	return 0;
}

static bool hello_field(struct json_reader *r, int which,
			struct lenslink_hello *h)
{
	switch (which) {
	case HELLO_NAME:
		/* Display only: a long name keeps its (UTF-8-safe) prefix. */
		read_string_field(r, h->name, sizeof(h->name), false);
		break;
	case HELLO_APP:
		read_string_field(r, h->app, sizeof(h->app), true);
		break;
	case HELLO_PROTOCOL: {
		double v;
		if (json_peek(r) != JSON_TYPE_NUMBER)
			return json_skip(r);
		if (!json_read_number(r, &v))
			return false;
		if (isfinite(v) && v >= 1.0 && v <= 255.0 && v == floor(v))
			h->protocol = (int)v;
		break;
	}
	case HELLO_KIND:
		h->is_screen = read_kind(r) == 1;
		break;
	case HELLO_STANDBY: {
		bool b;
		if (json_peek(r) != JSON_TYPE_BOOL)
			return json_skip(r);
		if (!json_read_bool(r, &b))
			return false;
		h->standby = b;
		break;
	}
	case HELLO_DEVICE_ID: {
		/* One byte past a UUID, so an over-long value reads as
		 * truncated rather than as a valid-looking prefix. */
		char raw[LENSLINK_DEVICE_ID_SIZE + 1];
		if (!read_string_field(r, raw, sizeof(raw), true) ||
		    !lenslink_device_id_parse(raw, strlen(raw),
					      h->device_id)) {
			h->device_id[0] = 0;
			h->device_id_invalid = true;
		}
		break;
	}
	default:
		return json_skip(r);
	}
	return !r->error;
}

bool lenslink_hello_parse(const char *json, size_t len,
			  struct lenslink_hello *out)
{
	memset(out, 0, sizeof(*out));
	if (!json || len > LENSLINK_HANDSHAKE_MAX)
		return false;

	struct json_reader r;
	json_reader_init(&r, json, len);
	if (!json_object_begin(&r))
		return false;

	struct lenslink_hello h;
	memset(&h, 0, sizeof(h));
	unsigned seen = 0;
	char key[KEY_SIZE];
	bool key_truncated;
	bool ok = true;

	while (ok && json_object_next(&r, key, sizeof(key), &key_truncated)) {
		int which = key_truncated ? 0 : hello_key(key);
		if (which && (seen & (unsigned)which)) {
			ok = false; /* duplicate known key: ambiguous */
			break;
		}
		seen |= (unsigned)which;
		ok = hello_field(&r, which, &h);
	}
	if (!ok || !json_reader_done(&r))
		return false;

	h.valid = true;
	*out = h;
	return true;
}

bool lenslink_video_config_parse(const char *json, size_t len,
				 struct lenslink_video_config *out)
{
	memset(out, 0, sizeof(*out));
	if (!json || len > LENSLINK_HANDSHAKE_MAX)
		return false;

	struct json_reader r;
	json_reader_init(&r, json, len);
	if (!json_object_begin(&r))
		return false;

	struct lenslink_video_config cfg;
	memset(&cfg, 0, sizeof(cfg));
	bool seen_codec = false, seen_kind = false;
	char key[KEY_SIZE];
	bool key_truncated;
	bool ok = true;

	while (ok && json_object_next(&r, key, sizeof(key), &key_truncated)) {
		if (!key_truncated && strcmp(key, "codec") == 0) {
			char codec[16];
			if (seen_codec) {
				ok = false;
				break;
			}
			seen_codec = true;
			if (read_string_field(&r, codec, sizeof(codec), true) &&
			    strcmp(codec, "hevc") == 0)
				cfg.codec = LENSLINK_CODEC_HEVC;
		} else if (!key_truncated && strcmp(key, "kind") == 0) {
			if (seen_kind) {
				ok = false;
				break;
			}
			seen_kind = true;
			int k = read_kind(&r);
			cfg.has_kind = k >= 0;
			cfg.is_screen = k == 1;
		} else {
			json_skip(&r);
		}
		ok = ok && !r.error;
	}
	if (!ok || !json_reader_done(&r))
		return false;

	cfg.valid = true;
	*out = cfg;
	return true;
}
