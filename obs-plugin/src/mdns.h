/*
 * Minimal one-shot mDNS-SD browser: finds LensLink phones on the LAN so
 * the user picks a name instead of typing an IP, and resolves a bound
 * source's persistent device id to the phone's current address. See
 * mdns.c for the design constraints (no port-5353 bind, no multicast
 * membership) and mdns-parse.h for what is read from a reply.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "mdns-parse.h"

/* Blocking browse for `service_type` (e.g. "_lenslink._tcp.local") lasting
 * about `timeout_ms`. Fills up to `max_results` unique hosts; returns the
 * number found. Safe to call from any thread. */
int mdns_browse(const char *service_type, int timeout_ms,
		struct mdns_result *results, int max_results);

/* Browses like mdns_browse but returns as soon as a responder advertises
 * `device_id` (canonical), copying its entry to `out`. Gives up after
 * `timeout_ms`, or within ~100 ms of `*stop` becoming true. Returns true
 * when found. The result is only a hint: the caller still verifies the
 * phone's HELLO. */
bool mdns_resolve_device(const char *service_type, const char *device_id,
			 int timeout_ms, volatile bool *stop,
			 struct mdns_result *out);
