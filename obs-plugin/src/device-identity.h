/*
 * Persistent LensLink device identity (docs/PROTOCOL.md, "Device
 * identity").
 *
 * Every LensLink app installation owns one random UUID, its `deviceID`.
 * The app sends it in HELLO and advertises it in its Bonjour TXT record;
 * a source *bound* to a device stores it and accepts a connection only
 * when the HELLO repeats it. It is not the same thing as, and never
 * derived from:
 *
 *   - the source's runtime id (source-registry.h): session-local, opaque,
 *     assigned per OBS process — never persisted;
 *   - the OBS source name (the production role, e.g. "Stage Left");
 *   - the phone's display name (mutable; the user can rename the phone);
 *   - an IP address or host name (ephemeral; DHCP moves it).
 *
 * Pure C with no libobs dependency, so it is unit-tested standalone.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Canonical text form: 8-4-4-4-12 upper-case hex digits. */
#define LENSLINK_DEVICE_ID_LEN 36
#define LENSLINK_DEVICE_ID_SIZE (LENSLINK_DEVICE_ID_LEN + 1)

/* Validates `len` bytes (no NUL terminator needed; an embedded NUL is
 * just an invalid character) as a UUID in 8-4-4-4-12 hex form, either
 * case, and writes its canonical upper-case form to `out`. Anything
 * else — wrong length, separators, non-hex, surrounding whitespace or
 * braces, the all-zero nil UUID, the all-F max UUID — is rejected and
 * `out` is set to "". */
bool lenslink_device_id_parse(const char *s, size_t len,
			      char out[LENSLINK_DEVICE_ID_SIZE]);

/* The same check for a C string (NULL and "" are invalid). */
bool lenslink_device_id_valid(const char *s);

/* Where a source stands with respect to its bound device. */
enum lenslink_identity_state {
	/* Legacy source with no device id: whatever answers at its
	 * address is accepted, exactly as before identities existed. */
	LENSLINK_IDENTITY_UNBOUND,
	/* Bound; no verified connection at the moment. */
	LENSLINK_IDENTITY_SEARCHING,
	/* Bound; the connected phone's HELLO carried the expected id. */
	LENSLINK_IDENTITY_VERIFIED,
	/* Bound; the phone reached last announced a different id (e.g.
	 * another phone took the cached address). Rejected. */
	LENSLINK_IDENTITY_MISMATCH,
	/* Bound; the phone reached last announced no valid id (an app
	 * older than device identity, or a malformed HELLO). Rejected:
	 * it can't prove it is the bound phone. */
	LENSLINK_IDENTITY_UNIDENTIFIED,
};

/* The verdict on one HELLO. `expected`: the source's bound device id,
 * "" or NULL for an unbound source. `announced`: the HELLO's validated
 * deviceID, "" or NULL when absent or invalid. Returns UNBOUND (accept,
 * legacy), VERIFIED (accept), MISMATCH or UNIDENTIFIED (reject). Both ids
 * must already be canonical (lenslink_device_id_parse). */
enum lenslink_identity_state lenslink_identity_verify(const char *expected,
						      const char *announced);

static inline bool lenslink_identity_accepted(enum lenslink_identity_state v)
{
	return v == LENSLINK_IDENTITY_UNBOUND ||
	       v == LENSLINK_IDENTITY_VERIFIED;
}

/* Stable lower-case name for logs and diagnostics ("unbound",
 * "searching", "verified", "mismatch", "unidentified"). */
const char *lenslink_identity_state_name(enum lenslink_identity_state st);

#ifdef __cplusplus
}
#endif
