#include "device-identity.h"

#include <string.h>

static int hex_digit(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

bool lenslink_device_id_parse(const char *s, size_t len,
			      char out[LENSLINK_DEVICE_ID_SIZE])
{
	static const char digits[] = "0123456789ABCDEF";
	char tmp[LENSLINK_DEVICE_ID_SIZE];
	bool all_zero = true, all_f = true;

	out[0] = 0;
	if (!s || len != LENSLINK_DEVICE_ID_LEN)
		return false;

	for (size_t i = 0; i < LENSLINK_DEVICE_ID_LEN; i++) {
		if (i == 8 || i == 13 || i == 18 || i == 23) {
			if (s[i] != '-')
				return false;
			tmp[i] = '-';
			continue;
		}
		int h = hex_digit(s[i]);
		if (h < 0)
			return false;
		all_zero = all_zero && h == 0;
		all_f = all_f && h == 15;
		tmp[i] = digits[h];
	}
	/* Nil and max UUIDs are placeholders, never a generated identity. */
	if (all_zero || all_f)
		return false;

	tmp[LENSLINK_DEVICE_ID_LEN] = 0;
	memcpy(out, tmp, sizeof(tmp));
	return true;
}

bool lenslink_device_id_valid(const char *s)
{
	char canonical[LENSLINK_DEVICE_ID_SIZE];
	return s && lenslink_device_id_parse(s, strlen(s), canonical);
}

enum lenslink_identity_state lenslink_identity_verify(const char *expected,
						      const char *announced)
{
	if (!expected || !expected[0])
		return LENSLINK_IDENTITY_UNBOUND;
	if (!announced || !announced[0])
		return LENSLINK_IDENTITY_UNIDENTIFIED;
	return strcmp(expected, announced) == 0 ? LENSLINK_IDENTITY_VERIFIED
						: LENSLINK_IDENTITY_MISMATCH;
}

const char *lenslink_identity_state_name(enum lenslink_identity_state st)
{
	switch (st) {
	case LENSLINK_IDENTITY_UNBOUND:
		return "unbound";
	case LENSLINK_IDENTITY_SEARCHING:
		return "searching";
	case LENSLINK_IDENTITY_VERIFIED:
		return "verified";
	case LENSLINK_IDENTITY_MISMATCH:
		return "mismatch";
	case LENSLINK_IDENTITY_UNIDENTIFIED:
		return "unidentified";
	}
	return "unknown";
}
