/*
 * Minimal one-shot mDNS-SD browser (RFC 6762 §5.1, "one-shot multicast
 * DNS queries"). Sends a PTR question for a service type to 224.0.0.251
 * from an *ephemeral* UDP port with the unicast-response (QU) bit set;
 * per the RFC, responders reply via unicast straight to our port. That
 * sidesteps binding port 5353 or joining the multicast group — both of
 * which fight the OS's own Bonjour/Avahi daemon — and makes the same
 * plain-socket code work on Windows, macOS, and Linux.
 *
 * Deliberately small: each phone's NAME, ADDRESS, and the identity in
 * its TXT record are all we need. The wire port is fixed by the
 * protocol, and the reply's source address IS the device, so SRV and A
 * records are ignored; mdns-parse.c pulls the PTR instance label and the
 * matching TXT metadata. Multicast queries go out the default-route
 * interface only — fine for the single-LAN setups LensLink targets.
 */

#include <stdbool.h> /* net-compat.h uses bool; include first */
#include <string.h>
#include <stdio.h>

#include "net-compat.h"
#include "mdns.h"

#include <util/platform.h>

/* Overridable so a test harness can point the query at loopback
 * (containers/CI often have no multicast route) and at a port the OS's
 * own responder doesn't hold. */
#ifndef MDNS_ADDR
#define MDNS_ADDR "224.0.0.251"
#endif
#ifndef MDNS_PORT
#define MDNS_PORT 5353
#endif
#define DNS_TYPE_PTR 12
#define DNS_CLASS_IN 1
#define DNS_UNICAST_RESPONSE 0x8000

/* The browse loop behind both entry points. With `want_id`, returns as
 * soon as that device's entry is in `results`. */
static int browse(const char *service_type, int timeout_ms,
		  struct mdns_result *results, int max_results,
		  volatile bool *stop, const char *want_id)
{
	uint8_t query[160];
	uint8_t qname[128];

	int name_len = mdns_encode_name(service_type, qname, sizeof(qname));
	if (name_len < 0 || max_results <= 0 || timeout_ms <= 0)
		return 0;

	memset(query, 0, 12);
	query[5] = 1; /* qdcount */
	memcpy(query + 12, qname, (size_t)name_len);
	size_t qlen = 12 + (size_t)name_len;
	query[qlen++] = 0;
	query[qlen++] = DNS_TYPE_PTR;
	query[qlen++] = (DNS_CLASS_IN | DNS_UNICAST_RESPONSE) >> 8;
	query[qlen++] = DNS_CLASS_IN & 0xFF;

	socket_t sock = socket(AF_INET, SOCK_DGRAM, 0);
	if (sock == OBSC_INVALID_SOCKET)
		return 0;
	net_set_nonblocking(sock);

	struct sockaddr_in dest = {0};
	dest.sin_family = AF_INET;
	dest.sin_port = htons(MDNS_PORT);
	inet_pton(AF_INET, MDNS_ADDR, &dest.sin_addr);

	int count = 0;
	uint64_t deadline =
		os_gettime_ns() + (uint64_t)timeout_ms * 1000000ULL;
	uint64_t next_send = 0;

	while (count < max_results && !(stop && *stop)) {
		uint64_t now = os_gettime_ns();
		if (now >= deadline)
			break;

		if (now >= next_send) {
			sendto(sock, (const char *)query, (int)qlen, 0,
			       (struct sockaddr *)&dest, sizeof(dest));
			/* One retransmit halfway through the window covers
			 * a lost first query. */
			next_send = now + (uint64_t)timeout_ms * 500000ULL;
		}

		int remaining_ms = (int)((deadline - now) / 1000000ULL);
		int wait_ms = remaining_ms < 100 ? remaining_ms : 100;
		if (net_wait(sock, NET_WAIT_READ, wait_ms > 0 ? wait_ms : 1) <=
		    0)
			continue;

		uint8_t buf[1500];
		struct sockaddr_in from;
		socklen_t from_len = sizeof(from);
		int n = (int)recvfrom(sock, (char *)buf, sizeof(buf), 0,
				      (struct sockaddr *)&from, &from_len);
		char host[64];
		if (n <= 0 || from.sin_family != AF_INET ||
		    !inet_ntop(AF_INET, &from.sin_addr, host, sizeof(host)))
			continue;
		count = mdns_parse_response(buf, (size_t)n, qname,
					    (size_t)name_len, host, results,
					    count, max_results);
		if (want_id && mdns_find_device(results, count, want_id))
			break;
	}

	net_close(sock);
	return count;
}

int mdns_browse(const char *service_type, int timeout_ms,
		struct mdns_result *results, int max_results)
{
	return browse(service_type, timeout_ms, results, max_results, NULL,
		      NULL);
}

bool mdns_resolve_device(const char *service_type, const char *device_id,
			 int timeout_ms, volatile bool *stop,
			 struct mdns_result *out)
{
	/* Room for every phone on a busy LAN, so another phone's replies
	 * can't crowd out the one we want. */
	struct mdns_result found[16];
	if (!device_id || !device_id[0])
		return false;
	int n = browse(service_type, timeout_ms, found, 16, stop, device_id);
	const struct mdns_result *r = mdns_find_device(found, n, device_id);
	if (r)
		*out = *r;
	return r != NULL;
}
