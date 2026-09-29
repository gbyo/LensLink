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
	/* Identity, under `lock` like the real source's status_mutex. */
	pthread_mutex_t lock;
	char device_id[LENSLINK_DEVICE_ID_SIZE];
	atomic_int binds;
	/* Phone telemetry, under `lock` — a STATE arriving on the dial
	 * loop while a dock row is being read. */
	struct lenslink_telemetry telemetry;
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
	pthread_mutex_lock(&s->lock);
	snprintf(out->device_id, sizeof(out->device_id), "%s", s->device_id);
	out->identity = s->device_id[0] ? LENSLINK_IDENTITY_SEARCHING
					: LENSLINK_IDENTITY_UNBOUND;
	out->telemetry = s->telemetry;
	pthread_mutex_unlock(&s->lock);
}

void ios_camera_copy_camera_state(struct ios_camera_source *s,
				  struct lenslink_camera_state *out)
{
	touch(s);
	memset(out, 0, sizeof(*out));
	out->valid = true;
	pthread_mutex_lock(&s->lock);
	out->telemetry = s->telemetry;
	pthread_mutex_unlock(&s->lock);
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

void ios_camera_bind_device(struct ios_camera_source *s,
			    const char *device_id)
{
	touch(s);
	pthread_mutex_lock(&s->lock);
	snprintf(s->device_id, sizeof(s->device_id), "%s", device_id);
	pthread_mutex_unlock(&s->lock);
	atomic_fetch_add(&s->binds, 1);
}

/* --------------------------------------------------------------------- */

/* One STATE's worth of telemetry, built so a torn read is detectable:
 * every field is a function of `n`. */
static struct lenslink_telemetry telemetry_for(unsigned n)
{
	struct lenslink_telemetry t = {0};
	t.has_battery_percent = true;
	t.battery_percent = (int)(n % 101);
	t.battery_state = (enum lenslink_battery_state)(1 + n % 3);
	t.has_low_power_mode = true;
	t.low_power_mode = n & 1;
	t.thermal_state = (enum lenslink_thermal_state)(1 + n % 4);
	t.system_pressure = (enum lenslink_system_pressure)(1 + n % 5);
	t.system_pressure_factors = n % 32;
	return t;
}

static bool telemetry_consistent(const struct lenslink_telemetry *t)
{
	if (!t->has_battery_percent)
		return t->system_pressure == LL_PRESSURE_UNKNOWN &&
		       !t->battery_percent;
	/* The test only writes n in [0, 100], so the percentage names n. */
	struct lenslink_telemetry want =
		telemetry_for((unsigned)t->battery_percent);
	return memcmp(&want, t, sizeof(want)) == 0;
}

static void set_telemetry(struct ios_camera_source *s,
			  const struct lenslink_telemetry *t)
{
	pthread_mutex_lock(&s->lock);
	s->telemetry = *t;
	pthread_mutex_unlock(&s->lock);
}

static struct ios_camera_source *make(const char *name, bool screen)
{
	struct ios_camera_source *s = calloc(1, sizeof(*s));
	snprintf(s->name, sizeof(s->name), "%s", name);
	s->screen = screen;
	pthread_mutex_init(&s->lock, NULL);
	s->telemetry = telemetry_for(0);
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

#define ID_A "B1092868-1234-5678-ABCD-0123456789AB"
#define ID_B "0F5E2D4C-9A8B-4C7D-8E6F-102030405060"

static void test_bind(void)
{
	struct ios_camera_source *a = make("Stage Left", false);
	uint32_t ia = lenslink_registry_add(a);
	struct lenslink_source_info info;

	/* Unbound by default. */
	CHECK(lenslink_source_get_info(ia, &info));
	CHECK(!info.device_id[0] &&
	      info.identity == LENSLINK_IDENTITY_UNBOUND);

	/* Bound: stored canonical (upper case). */
	CHECK(lenslink_source_bind_device(
		ia, "b1092868-1234-5678-abcd-0123456789ab"));
	CHECK(lenslink_source_get_info(ia, &info));
	CHECK_STR(info.device_id, ID_A);
	CHECK(info.identity == LENSLINK_IDENTITY_SEARCHING);

	/* Invalid ids never reach the source; the binding stands. */
	int binds = atomic_load(&a->binds);
	CHECK(!lenslink_source_bind_device(ia, "not-a-uuid"));
	CHECK(!lenslink_source_bind_device(
		ia, "00000000-0000-0000-0000-000000000000"));
	CHECK(!lenslink_source_bind_device(ia, ID_A " "));
	CHECK(atomic_load(&a->binds) == binds);
	CHECK(lenslink_source_get_info(ia, &info));
	CHECK_STR(info.device_id, ID_A);

	/* Unbind with "" or NULL. */
	CHECK(lenslink_source_bind_device(ia, ""));
	CHECK(lenslink_source_get_info(ia, &info) && !info.device_id[0]);
	CHECK(lenslink_source_bind_device(ia, ID_B));
	CHECK(lenslink_source_bind_device(ia, NULL));
	CHECK(lenslink_source_get_info(ia, &info) && !info.device_id[0]);

	/* The runtime id and the persistent id are unrelated: rebinding
	 * doesn't change the runtime id, and a gone runtime id can't be
	 * bound. */
	CHECK(lenslink_source_bind_device(ia, ID_B));
	CHECK(lenslink_source_get_info(ia, &info) && info.id == ia);
	destroy(a);
	CHECK(!lenslink_source_bind_device(ia, ID_A));
	CHECK(atomic_load(&g_dead_touches) == 0);
	free(a);
}

/* A source's telemetry reaches every read path whole, and per source. */
static void test_telemetry_passthrough(void)
{
	struct ios_camera_source *a = make("Stage Left", false);
	struct ios_camera_source *b = make("Crowd", false);
	uint32_t ia = lenslink_registry_add(a);
	uint32_t ib = lenslink_registry_add(b);
	struct lenslink_telemetry ta = telemetry_for(87);
	struct lenslink_telemetry tb = {0}; /* a phone that sends none */
	set_telemetry(a, &ta);
	set_telemetry(b, &tb);

	struct lenslink_source_info info;
	CHECK(lenslink_source_get_info(ia, &info));
	CHECK(memcmp(&info.telemetry, &ta, sizeof(ta)) == 0);
	CHECK(lenslink_source_get_info(ib, &info));
	CHECK(memcmp(&info.telemetry, &tb, sizeof(tb)) == 0);

	struct lenslink_source_info list[LENSLINK_MAX_SOURCES];
	size_t n = lenslink_source_enum(list, LENSLINK_MAX_SOURCES);
	CHECK(n == 2);
	CHECK(memcmp(&list[0].telemetry, &ta, sizeof(ta)) == 0);
	CHECK(memcmp(&list[1].telemetry, &tb, sizeof(tb)) == 0);

	struct lenslink_camera_state st;
	CHECK(lenslink_source_get_camera_state(ia, &st));
	CHECK(memcmp(&st.telemetry, &ta, sizeof(ta)) == 0);
	CHECK(!list[1].telemetry.has_battery_percent);

	destroy(a);
	destroy(b);
	free(a);
	free(b);
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
static atomic_int g_torn;

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
			if (lenslink_source_get_camera_state(id, &st) &&
			    !telemetry_consistent(&st.telemetry))
				atomic_fetch_add(&g_torn, 1);
			lenslink_source_copy_state_json(id, json,
							sizeof(json));
			lenslink_source_stream_command(id,
						       LENSLINK_STREAM_RESUME);
			lenslink_source_recalibrate(id);
			lenslink_source_get_obs_source(id);
			/* Identity fields are never torn: a bound id is
			 * whole and valid, and matches the state. */
			lenslink_source_bind_device(id, (id & 1) ? ID_A : "");
			struct lenslink_source_info info;
			if (lenslink_source_get_info(id, &info)) {
				if (!telemetry_consistent(&info.telemetry))
					atomic_fetch_add(&g_torn, 1);
				bool bound = info.device_id[0] != 0;
				if (bound && !lenslink_device_id_valid(
						     info.device_id))
					atomic_fetch_add(&g_torn, 1);
				if (bound != (info.identity !=
					      LENSLINK_IDENTITY_UNBOUND))
					atomic_fetch_add(&g_torn, 1);
			}
		}
		lenslink_source_resolve_camera(0);
	}
	free(list);
	return NULL;
}

/* The dial loop's side of a STATE: rewrites a live source's telemetry
 * (each write internally consistent) while the readers above look. */
static struct ios_camera_source *g_writer_target;

static void *telemetry_writer_thread(void *arg)
{
	(void)arg;
	for (unsigned n = 0; !atomic_load(&g_stop); n++) {
		struct lenslink_telemetry t = telemetry_for(n % 101);
		set_telemetry(g_writer_target, &t);
	}
	return NULL;
}

static void test_concurrent_churn(void)
{
	pthread_t readers[4], writer;
	atomic_store(&g_stop, false);
	struct ios_camera_source *steady = make("steady", false);
	lenslink_registry_add(steady);
	g_writer_target = steady;
	pthread_create(&writer, NULL, telemetry_writer_thread, NULL);
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
	pthread_join(writer, NULL);
	destroy(steady);
	free(steady);
	for (int round = 0; round < CHURN_ROUNDS; round++)
		free(graveyard[round]);
	free(graveyard);

	CHECK(atomic_load(&g_dead_touches) == 0);
	CHECK(atomic_load(&g_torn) == 0);
	CHECK(lenslink_source_camera_count() == 0);
}

int main(void)
{
	test_basics();
	test_bind();
	test_telemetry_passthrough();
	test_command_json();
	test_capacity();
	test_concurrent_churn();
	TEST_MAIN_END();
}
