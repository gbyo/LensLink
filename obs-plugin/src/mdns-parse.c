/*
 * mDNS-SD response parser: see mdns-parse.h. Every read is bounds-checked
 * against the packet length; names are rebuilt into a fixed 255-byte
 * wire buffer with a bounded number of compression jumps, so crafted
 * pointer loops and over-long names are rejected rather than followed.
 */

#include "mdns-parse.h"

#include <stdio.h>
#include <string.h>

#define DNS_TYPE_PTR 12
#define DNS_TYPE_TXT 16
#define DNS_MAX_JUMPS 16
#define DNS_MAX_NAME 255 /* RFC 1035 §3.1, wire form incl. the root */
#define DNS_FLAG_RESPONSE 0x80

int mdns_encode_name(const char *name, uint8_t *out, size_t out_size)
{
	size_t o = 0;

	while (*name) {
		const char *dot = strchr(name, '.');
		size_t label = dot ? (size_t)(dot - name) : strlen(name);
		if (label == 0 || label > 63 || o + label + 1 >= out_size)
			return -1;
		out[o++] = (uint8_t)label;
		memcpy(out + o, name, label);
		o += label;
		name += label;
		if (*name == '.')
			name++;
	}
	if (o + 1 > out_size)
		return -1;
	out[o++] = 0;
	return (int)o;
}

struct wire_name {
	uint8_t data[DNS_MAX_NAME];
	size_t len;
};

/* Reads the (possibly compressed) name at `off` into uncompressed wire
 * form. Returns the offset just past the name at its original site, or 0
 * on malformed input: out of bounds, a label over 63, a reserved label
 * type, too many jumps, or a name over 255 bytes. */
static size_t read_name(const uint8_t *msg, size_t len, size_t off,
			struct wire_name *out)
{
	size_t next = 0;
	int jumps = 0;

	out->len = 0;
	for (;;) {
		if (off >= len)
			return 0;
		uint8_t c = msg[off];
		if ((c & 0xC0) == 0xC0) {
			if (off + 1 >= len || ++jumps > DNS_MAX_JUMPS)
				return 0;
			if (!next)
				next = off + 2;
			off = ((size_t)(c & 0x3F) << 8) | msg[off + 1];
			continue;
		}
		if (c & 0xC0)
			return 0; /* 0x40/0x80: reserved label types */
		if (off + 1 + c > len || out->len + 1 + c > DNS_MAX_NAME)
			return 0;
		memcpy(out->data + out->len, msg + off, (size_t)c + 1);
		out->len += (size_t)c + 1;
		off += (size_t)c + 1;
		if (c == 0)
			return next ? next : off;
	}
}

static uint8_t ascii_lower(uint8_t c)
{
	return c >= 'A' && c <= 'Z' ? (uint8_t)(c + 32) : c;
}

/* DNS names compare case-insensitively (ASCII only), label by label. The
 * wire form carries the label lengths, so a byte-wise walk is exact —
 * a label containing '.' can't alias two labels. */
static bool name_equal(const uint8_t *a, size_t alen, const uint8_t *b,
		       size_t blen)
{
	if (alen != blen)
		return false;
	size_t i = 0;
	while (i < alen) {
		uint8_t n = a[i];
		if (b[i] != n || i + 1 + n > alen)
			return false;
		for (size_t k = i + 1; k <= i + n; k++)
			if (ascii_lower(a[k]) != ascii_lower(b[k]))
				return false;
		i += (size_t)n + 1;
	}
	return true;
}

/* Copies `n` raw bytes as valid, printable UTF-8: malformed sequences and
 * control characters (C0, DEL) become U+FFFD, and truncation to `size`
 * never splits a code point. A label is raw bytes — it may hold a NUL,
 * which would otherwise end the C string early. */
static void copy_display_utf8(char *out, size_t size, const uint8_t *s,
			      size_t n)
{
	static const char repl[] = "\xEF\xBF\xBD";
	size_t o = 0;

	if (!size)
		return;
	for (size_t i = 0; i < n;) {
		uint8_t c = s[i];
		size_t seq = c < 0x80   ? 1
			     : c < 0xC2 ? 0
			     : c < 0xE0 ? 2
			     : c < 0xF0 ? 3
			     : c < 0xF5 ? 4
					: 0;
		bool ok = seq > 0 && i + seq <= n;
		for (size_t k = 1; ok && k < seq; k++)
			ok = (s[i + k] & 0xC0) == 0x80;
		/* Overlongs and surrogates in the 3/4-byte forms. */
		if (ok && seq == 3)
			ok = !(c == 0xE0 && s[i + 1] < 0xA0) &&
			     !(c == 0xED && s[i + 1] >= 0xA0);
		if (ok && seq == 4)
			ok = !(c == 0xF0 && s[i + 1] < 0x90) &&
			     !(c == 0xF4 && s[i + 1] >= 0x90);
		if (ok && seq == 1 && (c < 0x20 || c == 0x7F))
			ok = false;

		const char *src = ok ? (const char *)s + i : repl;
		size_t len = ok ? seq : 3;
		if (o + len + 1 > size)
			break;
		memcpy(out + o, src, len);
		o += len;
		i += ok ? seq : 1;
	}
	out[o] = 0;
}

static bool key_is(const uint8_t *key, size_t n, const char *want)
{
	size_t wn = strlen(want);
	if (n != wn)
		return false;
	for (size_t i = 0; i < n; i++)
		if (ascii_lower(key[i]) != (uint8_t)want[i])
			return false;
	return true;
}

bool mdns_parse_txt(const uint8_t *rdata, size_t len, struct mdns_result *r)
{
	enum { SEEN_ID = 1, SEEN_PROTO = 2, SEEN_KIND = 4 };
	unsigned seen = 0;
	bool id_dup = false;
	char id[LENSLINK_DEVICE_ID_SIZE] = "";
	int proto = 0;
	enum mdns_kind kind = MDNS_KIND_UNKNOWN;

	for (size_t off = 0; off < len;) {
		size_t n = rdata[off];
		if (off + 1 + n > len)
			return false; /* a string runs past the record */
		const uint8_t *s = rdata + off + 1;
		off += 1 + n;

		const uint8_t *eq = n ? memchr(s, '=', n) : NULL;
		size_t klen = eq ? (size_t)(eq - s) : n;
		if (klen == 0)
			continue; /* empty string, or no key: ignored */
		bool printable = true;
		for (size_t i = 0; i < klen; i++)
			printable = printable && s[i] >= 0x20 && s[i] <= 0x7E;
		if (!printable)
			continue;
		const uint8_t *val = eq ? eq + 1 : NULL;
		size_t vlen = eq ? n - klen - 1 : 0;

		if (key_is(s, klen, "id")) {
			if (seen & SEEN_ID) {
				id_dup = true;
				continue;
			}
			seen |= SEEN_ID;
			if (val)
				lenslink_device_id_parse((const char *)val,
							 vlen, id);
		} else if (key_is(s, klen, "proto")) {
			if (seen & SEEN_PROTO)
				continue;
			seen |= SEEN_PROTO;
			int v = 0;
			bool ok = val && vlen >= 1 && vlen <= 3;
			for (size_t i = 0; ok && i < vlen; i++) {
				ok = val[i] >= '0' && val[i] <= '9';
				v = v * 10 + (val[i] - '0');
			}
			if (ok && v >= 1 && v <= 255)
				proto = v;
		} else if (key_is(s, klen, "kind")) {
			if (seen & SEEN_KIND)
				continue;
			seen |= SEEN_KIND;
			if (val && vlen == 6 && memcmp(val, "camera", 6) == 0)
				kind = MDNS_KIND_CAMERA;
			else if (val && vlen == 6 &&
				 memcmp(val, "screen", 6) == 0)
				kind = MDNS_KIND_SCREEN;
		}
	}

	memcpy(r->device_id, id_dup ? "" : id,
	       id_dup ? 1 : sizeof(r->device_id));
	r->protocol = proto;
	r->kind = kind;
	return true;
}

static uint16_t be16(const uint8_t *p)
{
	return (uint16_t)(((unsigned)p[0] << 8) | p[1]);
}

int mdns_parse_response(const uint8_t *msg, size_t len,
			const uint8_t *service_wire, size_t service_wire_len,
			const char *from_host, struct mdns_result *results,
			int count, int max_results)
{
	struct wire_name instances[MDNS_MAX_INSTANCES];
	int instance_count = 0;
	struct {
		struct wire_name owner;
		size_t rdata, rdlen;
	} txts[MDNS_MAX_INSTANCES];
	int txt_count = 0;
	struct wire_name name;

	if (len < 12 || !(msg[2] & DNS_FLAG_RESPONSE) || !from_host ||
	    !from_host[0] || strlen(from_host) >= sizeof(results[0].host))
		return count;

	unsigned qdcount = be16(msg + 4);
	unsigned records = (unsigned)be16(msg + 6) + be16(msg + 8) +
			   be16(msg + 10);
	if (records > MDNS_MAX_RECORDS)
		records = MDNS_MAX_RECORDS;

	size_t off = 12;
	for (unsigned i = 0; i < qdcount; i++) {
		off = read_name(msg, len, off, &name);
		if (!off || off + 4 > len)
			return count;
		off += 4;
	}

	for (unsigned i = 0; i < records; i++) {
		off = read_name(msg, len, off, &name);
		if (!off || off + 10 > len)
			break; /* keep what the intact records gave */
		unsigned type = be16(msg + off);
		uint32_t ttl = ((uint32_t)msg[off + 4] << 24) |
			       ((uint32_t)msg[off + 5] << 16) |
			       ((uint32_t)msg[off + 6] << 8) | msg[off + 7];
		size_t rdlen = be16(msg + off + 8);
		size_t rdata = off + 10;
		if (rdata + rdlen > len)
			break;
		off = rdata + rdlen;

		if (type == DNS_TYPE_PTR && ttl > 0 &&
		    name_equal(name.data, name.len, service_wire,
			       service_wire_len) &&
		    instance_count < MDNS_MAX_INSTANCES) {
			/* TTL 0 is a goodbye: the service is going away. */
			struct wire_name *inst = &instances[instance_count];
			size_t end = read_name(msg, len, rdata, inst);
			if (end && end <= rdata + rdlen && inst->len > 1)
				instance_count++;
		} else if (type == DNS_TYPE_TXT &&
			   txt_count < MDNS_MAX_INSTANCES) {
			txts[txt_count].owner = name;
			txts[txt_count].rdata = rdata;
			txts[txt_count].rdlen = rdlen;
			txt_count++;
		}
	}
	if (!instance_count)
		return count;

	/* One entry per responder address: its first instance that has
	 * usable LensLink TXT metadata, else its first instance. */
	struct mdns_result found;
	bool have = false;
	for (int i = 0; i < instance_count; i++) {
		struct mdns_result cand;
		memset(&cand, 0, sizeof(cand));
		copy_display_utf8(cand.name, sizeof(cand.name),
				  instances[i].data + 1, instances[i].data[0]);
		snprintf(cand.host, sizeof(cand.host), "%s", from_host);
		for (int t = 0; t < txt_count; t++) {
			if (name_equal(txts[t].owner.data, txts[t].owner.len,
				       instances[i].data, instances[i].len)) {
				mdns_parse_txt(msg + txts[t].rdata,
					       txts[t].rdlen, &cand);
				break; /* first TXT for the owner */
			}
		}
		if (!have || (!found.device_id[0] && cand.device_id[0])) {
			found = cand;
			have = true;
		}
		if (found.device_id[0])
			break;
	}

	for (int i = 0; i < count; i++) {
		struct mdns_result *r = &results[i];
		if (strcmp(r->host, from_host) != 0)
			continue;
		if (!r->device_id[0] && found.device_id[0]) {
			memcpy(r->device_id, found.device_id,
			       sizeof(r->device_id));
			r->protocol = found.protocol;
			r->kind = found.kind;
		}
		return count;
	}
	if (count < max_results)
		results[count++] = found;
	return count;
}

const struct mdns_result *mdns_find_device(const struct mdns_result *results,
					   int count, const char *device_id)
{
	if (!device_id || !device_id[0])
		return NULL;
	for (int i = 0; i < count; i++)
		if (strcmp(results[i].device_id, device_id) == 0)
			return &results[i];
	return NULL;
}
