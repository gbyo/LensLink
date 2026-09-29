/*
 * Source-registry lifetime tests, against a stub provider standing in for
 * ios-camera-source.c. The property that matters: once
 * lenslink_registry_remove returns, no registry call ever touches that
 * source again — checked here by poisoning a removed source and having
 * every upcall assert it isn't poisoned, while other threads hammer the
 * public API.
 */

#include "source-registry.h"
#include "test.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>

struct ios_camera_source {
	char name[32];
	bool screen;
	atomic_bool dead;
	atomic_int controls;
	atomic_int recalibrations;
	atomic_bool auto_start;
};

static atomic_int g_dead_touches;

static void touch(struct ios_camera_source *s)
{
	if (atomic_load(&s->dead))
		atomic_fetch_add(&g_dead_touches, 1);
}

/* --- stub provider --------------------------------------------------- */

bool ios_camera_is_screen_source(struct ios_camera_source *s)
{
	touch(s);
	return s->screen;
}

void ios_camera_fill_info(struct ios_camera_source *s,
			  struct lenslink_source_info *out)
{
	touch(s);
	snprintf(out->source_name, sizeof(out->source_name), "%s", s->name);
	out->auto_start = atomic_load(&s->auto_start);
}

void ios_camera_copy_camera_state(struct ios_camera_source *s,
				  struct lenslink_camera_state *out)
{
	touch(s);
	memset(out, 0, sizeof(*out));
	out->valid = true;
}

void ios_camera_copy_state(struct ios_camera_source *s, char *buf,
			   size_t size)
{
	touch(s);
	snprintf(buf, size, "{\"src\":\"%s\"}", s->name);
}

struct obs_source *ios_camera_obs_source(struct ios_camera_source *s)
{
	touch(s);
	return (struct obs_source *)s; /* identity stand-in for a ref */
}

void ios_camera_enqueue_control(struct ios_camera_source *s, const char *json,
				size_t len)
{
	touch(s);
	(void)json;
	(void)len;
	atomic_fetch_add(&s->controls, 1);
}

void ios_camera_set_auto_start(struct ios_camera_source *s, bool on)
{
	touch(s);
	atomic_store(&s->auto_start, on);
}

void ios_camera_recalibrate(struct ios_camera_source *s)
{
	touch(s);
	atomic_fetch_add(&s->recalibrations, 1);
}

/* --------------------------------------------------------------------- */

static struct ios_camera_source *make(const char *name, bool screen)
{
	struct ios_camera_source *s = calloc(1, sizeof(*s));
	snprintf(s->name, sizeof(s->name), "%s", name);
	s->screen = screen;
	return s;
}

static void destroy(struct ios_camera_source *s)
{
	lenslink_registry_remove(s);
	atomic_store(&s->dead, true); /* poison: registry must not touch */
}

static void test_basics(void)
{
	struct ios_camera_source *a = make("Stage Left", false);
	struct ios_camera_source *scr = make("Screen", true);
	struct ios_camera_source *b = make("Stage Right", false);

	CHECK(lenslink_source_camera_count() == 0);
	CHECK(lenslink_source_resolve_camera(0) == 0);

	uint32_t ia = lenslink_registry_add(a);
	uint32_t is = lenslink_registry_add(scr);
	uint32_t ib = lenslink_registry_add(b);
	CHECK(ia && is && ib && ia != is && is != ib && ia != ib);
	CHECK(lenslink_registry_add(a) == ia); /* idempotent */
	CHECK(lenslink_source_camera_count() == 2);

	/* Creation order; ids and the fixed type flag are the registry's. */
	struct lenslink_source_info list[LENSLINK_MAX_SOURCES];
	size_t n = lenslink_source_enum(list, LENSLINK_MAX_SOURCES);
	CHECK(n == 3);
	CHECK(list[0].id == ia && list[1].id == is && list[2].id == ib);
	CHECK_STR(list[0].source_name, "Stage Left");
	CHECK(!list[0].is_screen && list[1].is_screen);
	CHECK_STR(list[0].sync, "off"); /* never NULL */
	CHECK(lenslink_source_enum(list, 1) == 1);

	/* The web panel's ?src= fallback: a camera id resolves to itself;
	 * a screen id, 0 or a bogus id fall back to the first camera. */
	CHECK(lenslink_source_resolve_camera(ib) == ib);
	CHECK(lenslink_source_resolve_camera(is) == ia);
	CHECK(lenslink_source_resolve_camera(0) == ia);
	CHECK(lenslink_source_resolve_camera(9999) == ia);

	CHECK(lenslink_source_stream_command(ib, LENSLINK_STREAM_PAUSE));
	CHECK(atomic_load(&b->controls) == 1);
	CHECK(lenslink_source_recalibrate(ia));
	CHECK(atomic_load(&a->recalibrations) == 1);
	CHECK(lenslink_source_set_auto_start(ia, true));
	struct lenslink_source_info info;
	CHECK(lenslink_source_get_info(ia, &info) && info.auto_start);
	char json[64];
	CHECK(lenslink_source_copy_state_json(ib, json, sizeof(json)));
	CHECK_STR(json, "{\"src\":\"Stage Right\"}");
	CHECK(lenslink_source_get_obs_source(ia) == (struct obs_source *)a);
	CHECK(!lenslink_source_send_control(ia, "", 0));

	/* Removed: every call on the old id fails; ids are never reused. */
	destroy(a);
	CHECK(!lenslink_source_get_info(ia, &info));
	CHECK(!lenslink_source_recalibrate(ia));
	CHECK(!lenslink_source_stream_command(ia, LENSLINK_STREAM_STOP));
	CHECK(!lenslink_source_copy_state_json(ia, json, sizeof(json)));
	CHECK(lenslink_source_get_obs_source(ia) == NULL);
	struct lenslink_camera_state st;
	CHECK(!lenslink_source_get_camera_state(ia, &st) && !st.valid);
	CHECK(lenslink_source_resolve_camera(ia) == ib);
	CHECK(lenslink_source_camera_count() == 1);

	struct ios_camera_source *c = make("Wide", false);
	uint32_t ic = lenslink_registry_add(c);
	CHECK(ic != ia && ic > ib);

	destroy(b);
	destroy(scr);
	destroy(c);
	CHECK(lenslink_source_enum(list, LENSLINK_MAX_SOURCES) == 0);
	CHECK(atomic_load(&g_dead_touches) == 0);
	free(a);
	free(b);
	free(scr);
	free(c);
}

static void test_command_json(void)
{
	CHECK_STR(lenslink_stream_command_json(LENSLINK_STREAM_START),
		  "{\"cmd\":\"start_stream\"}");
	CHECK_STR(lenslink_stream_command_json(LENSLINK_STREAM_STOP),
		  "{\"cmd\":\"stop_stream\"}");
	CHECK_STR(lenslink_stream_command_json(LENSLINK_STREAM_PAUSE),
		  "{\"cmd\":\"pause_stream\"}");
	CHECK_STR(lenslink_stream_command_json(LENSLINK_STREAM_RESUME),
		  "{\"cmd\":\"resume_stream\"}");
}

static void test_capacity(void)
{
	struct ios_camera_source *all[LENSLINK_MAX_SOURCES + 1];
	for (int i = 0; i <= LENSLINK_MAX_SOURCES; i++) {
		all[i] = make("cam", false);
		uint32_t id = lenslink_registry_add(all[i]);
		CHECK((i < LENSLINK_MAX_SOURCES) == (id != 0));
	}
	struct lenslink_source_info *list =
		malloc(sizeof(*list) * (LENSLINK_MAX_SOURCES + 1));
	CHECK(lenslink_source_enum(list, LENSLINK_MAX_SOURCES + 1) ==
	      LENSLINK_MAX_SOURCES);
	free(list);
	for (int i = 0; i <= LENSLINK_MAX_SOURCES; i++) {
		destroy(all[i]); /* removing an unregistered one is a no-op */
		free(all[i]);
	}
	CHECK(lenslink_source_camera_count() == 0);
}

/* --- concurrency ------------------------------------------------------ */

#define CHURN_ROUNDS 20000
static atomic_bool g_stop;

static void *reader_thread(void *arg)
{
	(void)arg;
	struct lenslink_source_info *list =
		malloc(sizeof(*list) * LENSLINK_MAX_SOURCES);
	struct lenslink_camera_state st;
	char json[64];
	while (!atomic_load(&g_stop)) {
		size_t n = lenslink_source_enum(list, LENSLINK_MAX_SOURCES);
		for (size_t i = 0; i < n; i++) {
			uint32_t id = list[i].id;
			/* The id may die between enum and these calls: they
			 * must fail cleanly, never touch the corpse. */
			lenslink_source_get_camera_state(id, &st);
			lenslink_source_copy_state_json(id, json,
							sizeof(json));
			lenslink_source_stream_command(id,
						       LENSLINK_STREAM_RESUME);
			lenslink_source_recalibrate(id);
			lenslink_source_get_obs_source(id);
		}
		lenslink_source_resolve_camera(0);
	}
	free(list);
	return NULL;
}

static void test_concurrent_churn(void)
{
	pthread_t readers[4];
	atomic_store(&g_stop, false);
	for (int i = 0; i < 4; i++)
		pthread_create(&readers[i], NULL, reader_thread, NULL);

	/* Poisoned sources are kept (not freed) until the readers stop, so
	 * a bug shows up as a counted dead touch rather than a crash. */
	struct ios_camera_source **graveyard =
		malloc(sizeof(*graveyard) * CHURN_ROUNDS);
	struct ios_camera_source *live[4] = {0};
	for (int round = 0; round < CHURN_ROUNDS; round++) {
		int slot = round % 4;
		if (live[slot])
			destroy(live[slot]);
		struct ios_camera_source *s = make("churn", round % 3 == 0);
		lenslink_registry_add(s);
		graveyard[round] = s;
		live[slot] = s;
	}
	for (int slot = 0; slot < 4; slot++)
		destroy(live[slot]);

	atomic_store(&g_stop, true);
	for (int i = 0; i < 4; i++)
		pthread_join(readers[i], NULL);
	for (int round = 0; round < CHURN_ROUNDS; round++)
		free(graveyard[round]);
	free(graveyard);

	CHECK(atomic_load(&g_dead_touches) == 0);
	CHECK(lenslink_source_camera_count() == 0);
}

int main(void)
{
	test_basics();
	test_command_json();
	test_capacity();
	test_concurrent_churn();
	TEST_MAIN_END();
}
