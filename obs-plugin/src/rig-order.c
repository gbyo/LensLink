#include "rig-order.h"

#include <string.h>

static int find_known(const struct lenslink_rig_order *o, const char *uuid)
{
	for (size_t i = 0; i < o->count; i++)
		if (strcmp(o->known[i], uuid) == 0)
			return (int)i;
	return -1;
}

static void copy_uuid(char *dst, const char *src)
{
	size_t n = strlen(src);
	if (n > LENSLINK_UUID_LEN)
		n = LENSLINK_UUID_LEN;
	memcpy(dst, src, n);
	dst[n] = '\0';
}

static bool in_present(const char *const *present, size_t n, const char *uuid)
{
	for (size_t i = 0; i < n; i++)
		if (present[i] && strcmp(present[i], uuid) == 0)
			return true;
	return false;
}

void lenslink_rig_order_clear(struct lenslink_rig_order *o)
{
	o->count = 0;
}

void lenslink_rig_order_import(struct lenslink_rig_order *o,
			       const char *const *saved, size_t n)
{
	o->count = 0;
	for (size_t i = 0; i < n && o->count < LENSLINK_RIG_MAX; i++) {
		if (!saved[i] || !saved[i][0])
			continue;
		char tmp[LENSLINK_UUID_SIZE];
		copy_uuid(tmp, saved[i]);
		if (find_known(o, tmp) >= 0)
			continue;
		copy_uuid(o->known[o->count++], tmp);
	}
}

/* Appends `uuid`; when the list is full, first evicts the earliest entry
 * that is not present (there always is one: present <= registry max,
 * well under LENSLINK_RIG_MAX). */
static void append_known(struct lenslink_rig_order *o, const char *uuid,
			 const char *const *present, size_t n)
{
	if (o->count >= LENSLINK_RIG_MAX) {
		size_t victim = LENSLINK_RIG_MAX;
		for (size_t i = 0; i < o->count; i++) {
			if (!in_present(present, n, o->known[i])) {
				victim = i;
				break;
			}
		}
		if (victim == LENSLINK_RIG_MAX)
			return; /* unreachable in practice */
		memmove(o->known[victim], o->known[victim + 1],
			(o->count - victim - 1) * sizeof(o->known[0]));
		o->count--;
	}
	copy_uuid(o->known[o->count++], uuid);
}

size_t lenslink_rig_order_arrange(struct lenslink_rig_order *o,
				  const char *const *present, size_t n,
				  size_t *out_index)
{
	size_t written = 0;

	/* Register anything new first, in the order it is presented. */
	for (size_t i = 0; i < n; i++) {
		if (!present[i] || !present[i][0])
			continue;
		if (find_known(o, present[i]) < 0)
			append_known(o, present[i], present, n);
	}

	/* Then walk the known order and emit what is here. */
	for (size_t k = 0; k < o->count; k++) {
		for (size_t i = 0; i < n; i++) {
			if (present[i] && strcmp(present[i], o->known[k]) == 0) {
				out_index[written++] = i;
				break; /* first of any duplicates only */
			}
		}
	}
	return written;
}

bool lenslink_rig_order_move(struct lenslink_rig_order *o,
			     const char *const *present, size_t n,
			     const char *uuid, int delta)
{
	if (!uuid || (delta != -1 && delta != 1) || n > LENSLINK_RIG_MAX)
		return false;

	size_t index[LENSLINK_RIG_MAX];
	size_t slots = lenslink_rig_order_arrange(o, present, n, index);

	size_t at = slots;
	for (size_t k = 0; k < slots; k++) {
		if (strcmp(present[index[k]], uuid) == 0) {
			at = k;
			break;
		}
	}
	if (at == slots)
		return false;
	if ((delta < 0 && at == 0) || (delta > 0 && at + 1 == slots))
		return false;

	size_t other = delta < 0 ? at - 1 : at + 1;
	int a = find_known(o, present[index[at]]);
	int b = find_known(o, present[index[other]]);
	if (a < 0 || b < 0)
		return false;

	/* Swap the two cameras' positions in the full list, so any absent
	 * camera parked between them keeps its own slot. */
	char tmp[LENSLINK_UUID_SIZE];
	memcpy(tmp, o->known[a], sizeof(tmp));
	memcpy(o->known[a], o->known[b], sizeof(tmp));
	memcpy(o->known[b], tmp, sizeof(tmp));
	return true;
}

size_t lenslink_rig_order_export(struct lenslink_rig_order *o,
				 const char *const *present, size_t n,
				 char (*out)[LENSLINK_UUID_SIZE], size_t max)
{
	size_t index[LENSLINK_RIG_MAX];
	if (n > LENSLINK_RIG_MAX)
		n = LENSLINK_RIG_MAX;
	size_t slots = lenslink_rig_order_arrange(o, present, n, index);
	size_t written = 0;
	for (size_t k = 0; k < slots && written < max; k++)
		copy_uuid(out[written++], present[index[k]]);
	return written;
}
