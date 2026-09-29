/*
 * mDNS-SD response parsing for the one-shot LensLink browse (mdns.c).
 * Split out, with no libobs or socket dependency, so the parser of this
 * untrusted network input is unit-tested and fuzzed standalone.
 *
 * A reply is walked across all of its sections (answers, authority,
 * additionals): PTR records for the browsed service name the instances,
 * and each instance's TXT record — which responders put in the
 * additionals of a PTR answer (RFC 6763 §12.1) — carries the LensLink
 * metadata (docs/PROTOCOL.md, "Discovery"):
 *
 *   id=<deviceID>   the installation's persistent UUID
 *   proto=<n>       wire protocol version
 *   kind=camera|screen
 *
 * Everything from the packet is a hint: a discovered id only picks
 * which address to dial, and the phone must still prove it in HELLO.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "device-identity.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Records examined per packet, and PTR/TXT records remembered per packet
 * while matching them up; anything past these is ignored. */
#define MDNS_MAX_RECORDS 64
#define MDNS_MAX_INSTANCES 8

enum mdns_kind {
	MDNS_KIND_UNKNOWN, /* no TXT, no kind key, or an unknown value */
	MDNS_KIND_CAMERA,
	MDNS_KIND_SCREEN,
};

struct mdns_result {
	/* Service instance label — the phone's display name, e.g. "Emma's
	 * iPhone". Valid UTF-8, control characters replaced; truncated on
	 * a code point boundary. Never identity: renaming changes it. */
	char name[64];
	/* IPv4 of the responder (the phone), dotted quad. */
	char host[64];
	/* From TXT; "" when the responder published none or it was
	 * invalid (an app older than device identity publishes no TXT). */
	char device_id[LENSLINK_DEVICE_ID_SIZE];
	int protocol; /* 0 = unknown */
	enum mdns_kind kind;
};

/* Encodes a dotted name ("_lenslink._tcp.local") into DNS wire labels;
 * returns bytes written or -1. */
int mdns_encode_name(const char *name, uint8_t *out, size_t out_size);

/* Scans one response packet from `from_host` for PTR records of
 * `service_wire` (a wire-format name from mdns_encode_name) and merges
 * what it finds into `results[0..count)`: one entry per responder
 * address; a later packet from the same address can fill in a TXT the
 * earlier one lacked, never overwrite one it had. Returns the new count
 * (at most max_results). Malformed packets add nothing. */
int mdns_parse_response(const uint8_t *msg, size_t len,
			const uint8_t *service_wire, size_t service_wire_len,
			const char *from_host, struct mdns_result *results,
			int count, int max_results);

/* The entry advertising `device_id` (canonical), or NULL. When several
 * addresses claim the same id — a stale cache, or a spoof — the first
 * responder wins; HELLO verification settles the rest. */
const struct mdns_result *mdns_find_device(const struct mdns_result *results,
					   int count, const char *device_id);

/* Parses TXT rdata into `r`'s id/protocol/kind. Keys are matched
 * case-insensitively; per RFC 6763 §6.4 the first occurrence of a key
 * wins, except `id`, which is dropped entirely when it appears twice
 * (identity must not be ambiguous). False (and nothing filled) for
 * malformed rdata. Exposed for tests. */
bool mdns_parse_txt(const uint8_t *rdata, size_t len, struct mdns_result *r);

#ifdef __cplusplus
}
#endif
