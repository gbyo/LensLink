/*
 * The operator's camera order — the one thing on the dock that must never
 * move on its own.
 *
 * A camera keeps its slot (and so its number: 1, 2, 3 …) regardless of
 * connection state, tally, warnings, battery, latency or name. The order
 * is keyed by the OBS source's persistent UUID — never the source name
 * (renamable) and never LensLink's runtime id (session-local, opaque) —
 * and lives in the scene collection, so each collection has its own rig.
 *
 * `known` is the persisted-order list: every camera the dock has placed,
 * in slot order. Absent cameras keep their entry for the session (a
 * camera that drops out of the source list and returns — an undo, say —
 * gets its old slot back); they are dropped when the order is exported for
 * saving, so a deleted camera leaves no residue in the file. Cameras not
 * yet known are appended at the bottom, in the order they are first seen.
 *
 * Pure: strings in, indices out. The obs_data (de)serialisation lives in
 * frontend-ui.cpp.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "scene-walk.h" /* LENSLINK_UUID_SIZE */

#ifdef __cplusplus
extern "C" {
#endif

#define LENSLINK_RIG_MAX 64

struct lenslink_rig_order {
	char known[LENSLINK_RIG_MAX][LENSLINK_UUID_SIZE];
	size_t count;
};

void lenslink_rig_order_clear(struct lenslink_rig_order *o);

/* Replaces the order with a saved one (dropping blanks and duplicates). */
void lenslink_rig_order_import(struct lenslink_rig_order *o,
			       const char *const *saved, size_t n);

/* Display order for the `present` cameras: out_index[k] is the index into
 * `present` of the camera in slot k. Unknown cameras are appended to the
 * order (in `present` order). Duplicate or empty entries in `present` are
 * skipped. Returns the number of slots written (<= n); `out_index` needs
 * room for n. */
size_t lenslink_rig_order_arrange(struct lenslink_rig_order *o,
				  const char *const *present, size_t n,
				  size_t *out_index);

/* Swaps `uuid` with its neighbour in display order (delta -1 = up,
 * +1 = down). False if it is absent, at the edge, or delta is neither. */
bool lenslink_rig_order_move(struct lenslink_rig_order *o,
			     const char *const *present, size_t n,
			     const char *uuid, int delta);

/* The order to persist: the `present` cameras only, in slot order.
 * Returns the count written (<= max). */
size_t lenslink_rig_order_export(struct lenslink_rig_order *o,
				 const char *const *present, size_t n,
				 char (*out)[LENSLINK_UUID_SIZE], size_t max);

#ifdef __cplusplus
}
#endif
