/*
 * Typed parsers for the phone's small JSON handshake packets — HELLO
 * (type 1) and VIDEO_CONFIG (type 2) — on the bounded json-reader.c, the
 * same reader STATE uses. docs/PROTOCOL.md defines the fields.
 *
 * Failure is per field where it can be, whole where it must be:
 *
 *   - A known field with the wrong type or an unusable value (a kind that
 *     doesn't fit, a malformed deviceID, a string holding an escaped NUL)
 *     is treated as absent; the other fields are unaffected.
 *   - A syntax error, a payload over the size cap, a non-object, or a
 *     known key given twice (parsers disagree on which copy wins, so the
 *     sender's intent is ambiguous) invalidates the whole packet: every
 *     field reads as absent and `valid` is false. Nothing half-parsed
 *     leaks out.
 *
 * Unknown keys are skipped, so new fields stay backwards-compatible.
 * Pure C with no libobs dependency, so it is unit-tested standalone.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "device-identity.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Larger payloads are malformed. Real HELLOs are ~150 bytes. */
#define LENSLINK_HANDSHAKE_MAX 4096

struct lenslink_hello {
	bool valid; /* one well-formed JSON object within the size cap */

	/* Phone display name, for status lines only (it changes when the
	 * user renames the phone). Truncated on a UTF-8 boundary if long;
	 * "" when absent. */
	char name[128];
	char app[32]; /* "LensLink"; "" when absent or too long */
	int protocol; /* the sender's protocol version; 0 when absent */

	/* `kind`: "screen" is a screen mirror; "camera", absent, or any
	 * other value is a camera (the historical reading). */
	bool is_screen;

	/* "standby": true — the app is idle, waiting for start_stream.
	 * Only the literal true counts. */
	bool standby;

	/* The installation's persistent identity, canonical upper case;
	 * "" when absent or invalid. `device_id_invalid` distinguishes a
	 * present-but-malformed value (for logs); both mean "no identity". */
	char device_id[LENSLINK_DEVICE_ID_SIZE];
	bool device_id_invalid;
};

/* Parses `len` bytes (no NUL terminator needed). Always fills `out`;
 * returns out->valid. */
bool lenslink_hello_parse(const char *json, size_t len,
			  struct lenslink_hello *out);

enum lenslink_codec {
	LENSLINK_CODEC_H264, /* "h264", absent, or unknown (historical) */
	LENSLINK_CODEC_HEVC, /* "hevc" */
};

struct lenslink_video_config {
	bool valid;
	enum lenslink_codec codec;
	/* `kind` present with a usable value: then `is_screen` says which.
	 * Absent leaves the connection's kind (from HELLO) unchanged. */
	bool has_kind;
	bool is_screen;
};

bool lenslink_video_config_parse(const char *json, size_t len,
				 struct lenslink_video_config *out);

#ifdef __cplusplus
}
#endif
