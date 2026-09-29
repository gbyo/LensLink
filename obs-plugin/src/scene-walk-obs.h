/*
 * libobs adapter for scene-walk.h: walks a real OBS scene.
 *
 * Plain C over libobs only — no frontend, no Qt — so it builds everywhere
 * the plugin does and a headless libobs can exercise it with real scenes.
 * The caller supplies the scene source (the frontend's Preview scene, held
 * with a strong reference for the duration of the call).
 */

#pragma once

#include <obs.h>

#include "scene-walk.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Collects the UUIDs of every LensLink Camera/Screen source that is
 * visible through `scene_source`: present as a scene item, with the item
 * and every group / nested-scene item above it visible. `scene_source`
 * NULL, or a source that isn't a scene, is a valid empty result. Returns
 * false (empty set) when the graph could not be walked within the bounds
 * of scene-walk.h. Nothing retained after return. */
bool lenslink_preview_collect(obs_source_t *scene_source,
			      struct lenslink_uuid_set *out);

#ifdef __cplusplus
}
#endif
