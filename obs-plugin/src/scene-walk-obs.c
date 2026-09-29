#include <obs-module.h>
#include <string.h>

#include "scene-walk-obs.h"

extern struct obs_source_info ios_camera_source_info;
extern struct obs_source_info lenslink_screen_source_info;

static bool is_lenslink_source(const obs_source_t *source)
{
	const char *id = obs_source_get_id(source);
	return id && (strcmp(id, ios_camera_source_info.id) == 0 ||
		      strcmp(id, lenslink_screen_source_info.id) == 0);
}

struct enum_param {
	bool (*cb)(void *param, const struct lenslink_walk_item *item);
	void *param;
};

static bool enum_trampoline(obs_scene_t *scene, obs_sceneitem_t *item,
			    void *p)
{
	UNUSED_PARAMETER(scene);
	struct enum_param *ep = p;

	/* The item is addref'd by the enumeration, and holds its source, so
	 * both are valid for this callback. */
	obs_source_t *source = obs_sceneitem_get_source(item);
	struct lenslink_walk_item out = {
		.visible = obs_sceneitem_visible(item),
	};

	if (obs_sceneitem_is_group(item)) {
		out.container = obs_group_from_source(source);
	} else if (obs_scene_from_source(source)) {
		out.container = obs_scene_from_source(source);
	} else if (is_lenslink_source(source)) {
		const char *uuid = obs_source_get_uuid(source);
		if (uuid && uuid[0])
			out.lenslink_uuid = uuid;
	}
	return ep->cb(ep->param, &out);
}

static void obs_enum_items(void *ctx, const void *container,
			   bool (*cb)(void *param,
				      const struct lenslink_walk_item *item),
			   void *param)
{
	UNUSED_PARAMETER(ctx);
	struct enum_param ep = {.cb = cb, .param = param};
	obs_scene_enum_items((obs_scene_t *)container, enum_trampoline, &ep);
}

bool lenslink_preview_collect(obs_source_t *scene_source,
			      struct lenslink_uuid_set *out)
{
	static const struct lenslink_walk_ops ops = {.enum_items =
							     obs_enum_items};
	obs_scene_t *root = obs_scene_from_source(scene_source);
	return lenslink_scene_walk(&ops, NULL, root, out);
}
