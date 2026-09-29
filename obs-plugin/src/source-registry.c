/*
 * Source registry: see source-registry.h for the contract.
 *
 * Deliberately free of libobs calls (everything source-specific goes
 * through the ios_camera_* provider upcalls), so the lifetime logic can be
 * unit-tested with a stub provider.
 */

#include "source-registry.h"

#include <util/threading.h>

#include <string.h>

static struct {
	pthread_mutex_t mutex;
	struct {
		uint32_t id;
		bool is_screen; /* source type; fixed at creation */
		struct ios_camera_source *src;
	} entries[LENSLINK_MAX_SOURCES];
	size_t count;
	uint32_t next_id;
} g_registry = {.mutex = PTHREAD_MUTEX_INITIALIZER, .next_id = 1};

/* Caller holds the mutex. */
static int locked_find(uint32_t id)
{
	if (!id)
		return -1;
	for (size_t i = 0; i < g_registry.count; i++)
		if (g_registry.entries[i].id == id)
			return (int)i;
	return -1;
}

uint32_t lenslink_registry_add(struct ios_camera_source *s)
{
	uint32_t id = 0;
	bool is_screen = ios_camera_is_screen_source(s);

	pthread_mutex_lock(&g_registry.mutex);
	for (size_t i = 0; i < g_registry.count; i++) {
		if (g_registry.entries[i].src == s) {
			id = g_registry.entries[i].id;
			pthread_mutex_unlock(&g_registry.mutex);
			return id;
		}
	}
	if (g_registry.count < LENSLINK_MAX_SOURCES) {
		id = g_registry.next_id++;
		if (!g_registry.next_id) /* 2^32 creations: skip 0 */
			g_registry.next_id = 1;
		g_registry.entries[g_registry.count].id = id;
		g_registry.entries[g_registry.count].is_screen = is_screen;
		g_registry.entries[g_registry.count].src = s;
		g_registry.count++;
	}
	pthread_mutex_unlock(&g_registry.mutex);
	return id;
}

void lenslink_registry_remove(struct ios_camera_source *s)
{
	pthread_mutex_lock(&g_registry.mutex);
	for (size_t i = 0; i < g_registry.count; i++) {
		if (g_registry.entries[i].src == s) {
			memmove(&g_registry.entries[i],
				&g_registry.entries[i + 1],
				(g_registry.count - i - 1) *
					sizeof(g_registry.entries[0]));
			g_registry.count--;
			break;
		}
	}
	pthread_mutex_unlock(&g_registry.mutex);
}

void lenslink_registry_for_each(void (*fn)(struct ios_camera_source *s,
					   void *param),
				void *param)
{
	pthread_mutex_lock(&g_registry.mutex);
	for (size_t i = 0; i < g_registry.count; i++)
		fn(g_registry.entries[i].src, param);
	pthread_mutex_unlock(&g_registry.mutex);
}

/* ------------------------------------------------------------------ */

static void fill(size_t index, struct lenslink_source_info *out)
{
	memset(out, 0, sizeof(*out));
	ios_camera_fill_info(g_registry.entries[index].src, out);
	out->id = g_registry.entries[index].id;
	out->is_screen = g_registry.entries[index].is_screen;
	if (!out->sync)
		out->sync = "off";
}

size_t lenslink_source_enum(struct lenslink_source_info *out, size_t max)
{
	size_t n = 0;
	pthread_mutex_lock(&g_registry.mutex);
	for (size_t i = 0; i < g_registry.count && n < max; i++)
		fill(i, &out[n++]);
	pthread_mutex_unlock(&g_registry.mutex);
	return n;
}

bool lenslink_source_get_info(uint32_t id, struct lenslink_source_info *out)
{
	pthread_mutex_lock(&g_registry.mutex);
	int i = locked_find(id);
	if (i >= 0)
		fill((size_t)i, out);
	pthread_mutex_unlock(&g_registry.mutex);
	return i >= 0;
}

bool lenslink_source_get_camera_state(uint32_t id,
				      struct lenslink_camera_state *out)
{
	pthread_mutex_lock(&g_registry.mutex);
	int i = locked_find(id);
	if (i >= 0)
		ios_camera_copy_camera_state(g_registry.entries[i].src, out);
	pthread_mutex_unlock(&g_registry.mutex);
	if (i < 0)
		memset(out, 0, sizeof(*out));
	return i >= 0;
}

bool lenslink_source_copy_state_json(uint32_t id, char *buf, size_t size)
{
	pthread_mutex_lock(&g_registry.mutex);
	int i = locked_find(id);
	if (i >= 0)
		ios_camera_copy_state(g_registry.entries[i].src, buf, size);
	pthread_mutex_unlock(&g_registry.mutex);
	return i >= 0;
}

uint32_t lenslink_source_resolve_camera(uint32_t id)
{
	uint32_t result = 0;
	pthread_mutex_lock(&g_registry.mutex);
	int i = locked_find(id);
	if (i >= 0 && !g_registry.entries[i].is_screen) {
		result = id;
	} else {
		for (size_t k = 0; k < g_registry.count; k++) {
			if (!g_registry.entries[k].is_screen) {
				result = g_registry.entries[k].id;
				break;
			}
		}
	}
	pthread_mutex_unlock(&g_registry.mutex);
	return result;
}

size_t lenslink_source_camera_count(void)
{
	size_t n = 0;
	pthread_mutex_lock(&g_registry.mutex);
	for (size_t i = 0; i < g_registry.count; i++)
		if (!g_registry.entries[i].is_screen)
			n++;
	pthread_mutex_unlock(&g_registry.mutex);
	return n;
}

struct obs_source *lenslink_source_get_obs_source(uint32_t id)
{
	struct obs_source *ref = NULL;
	pthread_mutex_lock(&g_registry.mutex);
	int i = locked_find(id);
	if (i >= 0)
		ref = ios_camera_obs_source(g_registry.entries[i].src);
	pthread_mutex_unlock(&g_registry.mutex);
	return ref;
}

/* ------------------------------------------------------------------ */

bool lenslink_source_send_control(uint32_t id, const char *json, size_t len)
{
	if (!json || !len)
		return false;
	pthread_mutex_lock(&g_registry.mutex);
	int i = locked_find(id);
	if (i >= 0)
		ios_camera_enqueue_control(g_registry.entries[i].src, json,
					   len);
	pthread_mutex_unlock(&g_registry.mutex);
	return i >= 0;
}

const char *lenslink_stream_command_json(enum lenslink_stream_command cmd)
{
	switch (cmd) {
	case LENSLINK_STREAM_START:
		return "{\"cmd\":\"start_stream\"}";
	case LENSLINK_STREAM_STOP:
		return "{\"cmd\":\"stop_stream\"}";
	case LENSLINK_STREAM_PAUSE:
		return "{\"cmd\":\"pause_stream\"}";
	case LENSLINK_STREAM_RESUME:
		return "{\"cmd\":\"resume_stream\"}";
	}
	return NULL;
}

bool lenslink_source_stream_command(uint32_t id,
				    enum lenslink_stream_command cmd)
{
	const char *json = lenslink_stream_command_json(cmd);
	return json && lenslink_source_send_control(id, json, strlen(json));
}

bool lenslink_source_set_auto_start(uint32_t id, bool on)
{
	pthread_mutex_lock(&g_registry.mutex);
	int i = locked_find(id);
	if (i >= 0)
		ios_camera_set_auto_start(g_registry.entries[i].src, on);
	pthread_mutex_unlock(&g_registry.mutex);
	return i >= 0;
}

bool lenslink_source_recalibrate(uint32_t id)
{
	pthread_mutex_lock(&g_registry.mutex);
	int i = locked_find(id);
	if (i >= 0)
		ios_camera_recalibrate(g_registry.entries[i].src);
	pthread_mutex_unlock(&g_registry.mutex);
	return i >= 0;
}
