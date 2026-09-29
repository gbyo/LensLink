#include "scene-walk.h"

#include <string.h>

bool lenslink_uuid_set_has(const struct lenslink_uuid_set *set,
			   const char *uuid)
{
	if (!set || !uuid)
		return false;
	for (size_t i = 0; i < set->count; i++)
		if (strcmp(set->uuid[i], uuid) == 0)
			return true;
	return false;
}

bool lenslink_uuid_set_add(struct lenslink_uuid_set *set, const char *uuid)
{
	if (!set || !uuid || !uuid[0])
		return false;
	if (lenslink_uuid_set_has(set, uuid))
		return true;
	if (set->count >= LENSLINK_WALK_MAX_SOURCES)
		return false;
	size_t n = strlen(uuid);
	if (n > LENSLINK_UUID_LEN)
		n = LENSLINK_UUID_LEN;
	memcpy(set->uuid[set->count], uuid, n);
	set->uuid[set->count][n] = '\0';
	set->count++;
	return true;
}

struct walk {
	const struct lenslink_walk_ops *ops;
	void *ctx;
	struct lenslink_uuid_set *out;

	const void *entered[LENSLINK_WALK_MAX_CONTAINERS];
	size_t entered_count;
	bool overflow;
};

static bool walk_container(struct walk *w, const void *container, int depth);

struct item_param {
	struct walk *w;
	int depth;
};

static bool walk_item(void *param, const struct lenslink_walk_item *item)
{
	struct item_param *p = param;
	struct walk *w = p->w;

	/* A hidden item hides everything under it: it is never entered. */
	if (!item->visible)
		return true;

	if (item->container) {
		if (!walk_container(w, item->container, p->depth + 1))
			return false;
	} else if (item->lenslink_uuid) {
		if (!lenslink_uuid_set_add(w->out, item->lenslink_uuid)) {
			w->overflow = true;
			return false;
		}
	}
	return !w->overflow;
}

static bool walk_container(struct walk *w, const void *container, int depth)
{
	if (depth > LENSLINK_WALK_MAX_DEPTH) {
		w->overflow = true;
		return false;
	}

	/* Already walked (a cycle, or one nested scene referenced twice):
	 * everything in it is already collected. Only containers actually
	 * entered are recorded, so reaching one first through a hidden item
	 * never suppresses a later visible reference. */
	for (size_t i = 0; i < w->entered_count; i++)
		if (w->entered[i] == container)
			return true;
	if (w->entered_count >= LENSLINK_WALK_MAX_CONTAINERS) {
		w->overflow = true;
		return false;
	}
	w->entered[w->entered_count++] = container;

	struct item_param p = {.w = w, .depth = depth};
	w->ops->enum_items(w->ctx, container, walk_item, &p);
	return !w->overflow;
}

bool lenslink_scene_walk(const struct lenslink_walk_ops *ops, void *ctx,
			 const void *root, struct lenslink_uuid_set *out)
{
	if (!out)
		return false;
	out->count = 0;
	if (!ops || !ops->enum_items)
		return false;
	if (!root)
		return true;

	struct walk w;
	memset(&w, 0, sizeof(w));
	w.ops = ops;
	w.ctx = ctx;
	w.out = out;

	walk_container(&w, root, 0);
	if (w.overflow) {
		out->count = 0;
		return false;
	}
	return true;
}
