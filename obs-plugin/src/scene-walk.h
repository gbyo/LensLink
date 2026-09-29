/*
 * "Which LensLink sources are actually visible through this scene?" — the
 * question behind a true Studio Mode Preview tally.
 *
 * `obs_source_showing()` cannot answer it: it is true for a projector, a
 * source interaction window, a rendered-but-hidden scene, or (later) the
 * dock's own camera preview, none of which is OBS Preview. So the dock
 * walks the Preview scene's graph itself and asks only whether an item
 * is *in* it and *visible* all the way up:
 *
 *   scene ──item(visible)──► LensLink source                 → found
 *         ──item(visible)──► nested scene ──item(visible)──► … recurse
 *         ──item(visible)──► group        ──item(visible)──► … recurse
 *         ──item(hidden) ──► anything                        → not entered
 *
 * The walk is defined over an abstract graph (`struct lenslink_walk_ops`)
 * so the libobs adapter (scene-walk-obs.c) stays a thin shim and the
 * logic — cycles, hidden groups, nested scenes, duplicates, bounds — is
 * unit-tested without a libobs. It is defensive by construction:
 *
 *   - a container is entered at most once (cycles and repeated references
 *     to one nested scene cost nothing; a container reached first through
 *     a hidden item is not marked visited, because a hidden item is never
 *     entered);
 *   - depth, containers entered and sources collected are all bounded;
 *     exceeding a bound means "cannot determine", and the caller gets
 *     `false` and an empty set — a missing PVW is better than a false one.
 *
 * The result holds UUID strings only. No OBS pointer outlives the walk.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "lenslink-uuid.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LENSLINK_WALK_MAX_DEPTH 16
#define LENSLINK_WALK_MAX_CONTAINERS 256
#define LENSLINK_WALK_MAX_SOURCES 64

struct lenslink_uuid_set {
	char uuid[LENSLINK_WALK_MAX_SOURCES][LENSLINK_UUID_SIZE];
	size_t count;
};

/* Adds `uuid` (copied) unless it is already there or the set is full.
 * False when full. */
bool lenslink_uuid_set_add(struct lenslink_uuid_set *set, const char *uuid);
bool lenslink_uuid_set_has(const struct lenslink_uuid_set *set,
			   const char *uuid);

/* One item directly inside a container, as the adapter reports it. */
struct lenslink_walk_item {
	bool visible; /* the item's own visibility flag */

	/* Non-NULL: the item is a nested scene or a group. It is the
	 * container's identity for cycle detection (any stable pointer the
	 * adapter can produce again for the same container). */
	const void *container;

	/* container == NULL: a plain source. Non-NULL only when it is a
	 * LensLink source, and then its OBS UUID (valid for the duration
	 * of the callback). Other sources are of no interest. */
	const char *lenslink_uuid;
};

struct lenslink_walk_ops {
	/* Calls `cb` for each item directly inside `container` until it
	 * returns false. Must not retain `item` or its strings. */
	void (*enum_items)(void *ctx, const void *container,
			   bool (*cb)(void *param,
				      const struct lenslink_walk_item *item),
			   void *param);
};

/* Collects the UUIDs of every LensLink source visible through `root`.
 * Returns true on a complete walk. Returns false — with `*out` emptied —
 * if a bound was exceeded, i.e. the answer is unknown. `root` NULL is a
 * valid walk with an empty result (no Preview scene). */
bool lenslink_scene_walk(const struct lenslink_walk_ops *ops, void *ctx,
			 const void *root, struct lenslink_uuid_set *out);

#ifdef __cplusplus
}
#endif
