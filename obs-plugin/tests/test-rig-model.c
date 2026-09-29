/*
 * The dock model (rig-model.h): Program / Preview / Selected staying
 * independent, stable numbering, warning priority and wording, the health
 * line's text, and the rig summary. Wording is checked against the real
 * en-US.ini, so a key the model asks for but the locale lacks fails here.
 */

#include "rig-model.h"
#include "test.h"

#include <stdlib.h>

/* --- the real English strings ---------------------------------------- */

#define MAX_STRINGS 256
static struct {
	char key[64];
	char value[512];
} g_strings[MAX_STRINGS];
static int g_string_count;
static int g_missing;

static void load_strings(void)
{
	FILE *f = fopen(LENSLINK_EN_INI, "rb");
	if (!f) {
		fprintf(stderr, "cannot open %s\n", LENSLINK_EN_INI);
		test_failures++;
		return;
	}
	char line[2048];
	while (fgets(line, sizeof(line), f) && g_string_count < MAX_STRINGS) {
		char *eq = strchr(line, '=');
		if (!eq || line[0] == '#')
			continue;
		*eq = '\0';
		char *v = eq + 1;
		size_t n = strlen(v);
		while (n && (v[n - 1] == '\n' || v[n - 1] == '\r'))
			v[--n] = '\0';
		if (n < 2 || v[0] != '"' || v[n - 1] != '"')
			continue;
		v[n - 1] = '\0';
		snprintf(g_strings[g_string_count].key, 64, "%s", line);
		snprintf(g_strings[g_string_count].value, 512, "%s", v + 1);
		g_string_count++;
	}
	fclose(f);
}

static const char *text(const char *key)
{
	for (int i = 0; i < g_string_count; i++)
		if (strcmp(g_strings[i].key, key) == 0)
			return g_strings[i].value;
	fprintf(stderr, "missing locale key: %s\n", key);
	g_missing++;
	return key;
}

/* --- fixtures -------------------------------------------------------- */

static struct lenslink_source_info cam(uint32_t id, const char *uuid,
				       const char *name)
{
	struct lenslink_source_info h;
	memset(&h, 0, sizeof(h));
	h.id = id;
	snprintf(h.uuid, sizeof(h.uuid), "%s", uuid);
	snprintf(h.source_name, sizeof(h.source_name), "%s", name);
	snprintf(h.device_name, sizeof(h.device_name), "Gibson's iPhone");
	snprintf(h.transport, sizeof(h.transport), "Wi-Fi");
	h.connected = true;
	h.tone = STATUS_TONE_LIVE;
	h.identity = LENSLINK_IDENTITY_UNBOUND;
	h.has_rates = true;
	h.fps = 60;
	h.bitrate_bps = 10.4e6;
	h.latency_ms = 41;
	return h;
}

static struct lenslink_source_info screen(uint32_t id, const char *uuid,
					  const char *name)
{
	struct lenslink_source_info h = cam(id, uuid, name);
	h.is_screen = true;
	return h;
}

static struct lenslink_rig g_rig;
static struct lenslink_rig_view *g_view;

static void build(const struct lenslink_source_info *s, size_t n,
		  const struct lenslink_uuid_set *preview)
{
	lenslink_rig_build(&g_rig, s, n, preview, g_view);
}

static const struct lenslink_card *find(const char *uuid)
{
	for (size_t i = 0; i < g_view->count; i++)
		if (strcmp(g_view->cards[i].uuid, uuid) == 0)
			return &g_view->cards[i];
	return NULL;
}

static void fresh(void)
{
	lenslink_rig_init(&g_rig, text);
}

/* --- Program / Preview / Selected ------------------------------------ */

static void test_tally_combinations(void)
{
	fresh();
	/* All eight combinations of (program, preview, selected). */
	struct lenslink_source_info s[8];
	struct lenslink_uuid_set preview;
	preview.count = 0;
	for (int i = 0; i < 8; i++) {
		char uuid[16];
		snprintf(uuid, sizeof(uuid), "cam-%d", i);
		s[i] = cam((uint32_t)i + 1, uuid, uuid);
		s[i].program = (i & 1) != 0;
		if (i & 2)
			lenslink_uuid_set_add(&preview, uuid);
	}
	build(s, 8, &preview);
	for (int i = 0; i < 8; i++) {
		char uuid[16];
		snprintf(uuid, sizeof(uuid), "cam-%d", i);
		CHECK(lenslink_rig_select(&g_rig, g_view, uuid));
		build(s, 8, &preview); /* selection lands on the next build */
		int n = 0;
		for (int k = 0; k < 8; k++) {
			snprintf(uuid, sizeof(uuid), "cam-%d", k);
			const struct lenslink_card *c = find(uuid);
			CHECK(c != NULL);
			CHECK(c->program == ((k & 1) != 0));
			CHECK(c->preview == ((k & 2) != 0));
			/* Selection is the only thing that follows the click. */
			CHECK(c->selected == (k == i));
			n += c->selected;
		}
		CHECK(n == 1); /* exactly one selected camera */
	}
}

static void test_selecting_touches_no_tally(void)
{
	fresh();
	struct lenslink_source_info s[3] = {cam(1, "A", "A"), cam(2, "B", "B"),
					    cam(3, "C", "C")};
	s[0].program = true;
	struct lenslink_uuid_set preview = {0};
	lenslink_uuid_set_add(&preview, "B");

	build(s, 3, &preview);
	CHECK(lenslink_rig_select(&g_rig, g_view, "C"));
	build(s, 3, &preview);
	CHECK(find("A")->program && !find("A")->preview);
	CHECK(!find("B")->program && find("B")->preview);
	CHECK(!find("C")->program && !find("C")->preview);
	CHECK(find("C")->selected);
	/* The model has no path back to OBS at all: the inputs are copies. */
}

static void test_program_and_preview_at_once(void)
{
	fresh();
	struct lenslink_source_info s[1] = {cam(1, "A", "A")};
	s[0].program = true;
	struct lenslink_uuid_set preview = {0};
	lenslink_uuid_set_add(&preview, "A");
	build(s, 1, &preview);
	CHECK(find("A")->program);
	CHECK(find("A")->preview); /* both facts survive */
}

static void test_showing_somewhere_is_not_preview(void)
{
	fresh();
	/* A projector (or the future camera-control preview) makes the
	 * registry say "preview" — showing but not program. The dock must
	 * not believe it. */
	struct lenslink_source_info s[1] = {cam(1, "A", "A")};
	s[0].preview = true;

	build(s, 1, NULL); /* Studio Mode off */
	CHECK(!find("A")->preview);

	struct lenslink_uuid_set empty = {0}; /* studio on, A not in it */
	build(s, 1, &empty);
	CHECK(!find("A")->preview);
}

static void test_program_is_libobs_active_state(void)
{
	fresh();
	/* Both sides of a running transition are active: both are PGM. */
	struct lenslink_source_info s[3] = {cam(1, "A", "A"), cam(2, "B", "B"),
					    cam(3, "C", "C")};
	s[0].program = true;
	s[1].program = true;
	build(s, 3, NULL);
	CHECK(find("A")->program && find("B")->program);
	CHECK(!find("C")->program);
}

/* --- order, numbering, selection lifecycle -------------------------- */

static void test_numbers_follow_slots_and_never_reshuffle(void)
{
	fresh();
	struct lenslink_source_info s[3] = {cam(1, "A", "Camera 1"),
					    cam(2, "B", "Camera 2"),
					    cam(3, "C", "Camera 3")};
	build(s, 3, NULL);
	CHECK(find("A")->number == 1 && find("B")->number == 2 &&
	      find("C")->number == 3);

	/* Every kind of change the operator sees, none may move a card. */
	s[1].program = true;
	s[2].connected = false;
	s[2].tone = STATUS_TONE_WAIT;
	s[0].telemetry.system_pressure = LL_PRESSURE_SERIOUS;
	s[0].telemetry.thermal_state = LL_THERMAL_SERIOUS;
	s[1].latency_ms = 900;
	snprintf(s[2].source_name, sizeof(s[2].source_name), "Aardvark");
	build(s, 3, NULL);
	CHECK_STR(g_view->cards[0].uuid, "A");
	CHECK_STR(g_view->cards[1].uuid, "B");
	CHECK_STR(g_view->cards[2].uuid, "C");
	CHECK_STR(g_view->cards[2].name, "Aardvark"); /* rename shows... */
	CHECK(find("C")->number == 3);                /* ...slot doesn't move */

	/* Registry order (creation order) differs next launch: same slots. */
	struct lenslink_source_info shuffled[3] = {s[2], s[0], s[1]};
	build(shuffled, 3, NULL);
	CHECK_STR(g_view->cards[0].uuid, "A");
	CHECK_STR(g_view->cards[1].uuid, "B");
	CHECK_STR(g_view->cards[2].uuid, "C");
}

static void test_saved_order_and_new_camera(void)
{
	fresh();
	const char *saved[] = {"B", "A", "C"};
	lenslink_rig_order_import(&g_rig.order, saved, 3);
	struct lenslink_source_info s[4] = {cam(1, "A", "a"), cam(2, "B", "b"),
					    cam(3, "C", "c"), cam(4, "D", "d")};
	build(s, 4, NULL);
	CHECK_STR(g_view->cards[0].uuid, "B");
	CHECK_STR(g_view->cards[1].uuid, "A");
	CHECK_STR(g_view->cards[2].uuid, "C");
	CHECK_STR(g_view->cards[3].uuid, "D");
	CHECK(find("D")->number == 4);
}

static void test_screens_are_not_cameras(void)
{
	fresh();
	struct lenslink_source_info s[4] = {
		screen(1, "S1", "Phone screen"), cam(2, "A", "Cam A"),
		cam(3, "B", "Cam B"), screen(4, "S2", "iPad")};
	build(s, 4, NULL);
	CHECK(g_view->camera_count == 2);
	CHECK(g_view->count == 4);
	CHECK(g_view->summary.cameras == 2);
	CHECK(g_view->summary.screens == 2);
	CHECK(find("A")->number == 1 && find("B")->number == 2);
	CHECK(find("S1")->number == 0 && find("S2")->number == 0);
	CHECK(g_view->cards[0].is_screen == false); /* cameras first */
	CHECK(g_view->cards[2].is_screen == true);
	/* A screen can't be chosen for camera control. */
	CHECK(!lenslink_rig_select(&g_rig, g_view, "S1"));
	CHECK(!find("S1")->can_pause && !find("S1")->can_stop &&
	      !find("S1")->can_recalibrate);
}

static void test_selection_survives_rename_and_status(void)
{
	fresh();
	struct lenslink_source_info s[2] = {cam(1, "A", "Camera 1"),
					    cam(2, "B", "Camera 2")};
	build(s, 2, NULL);
	CHECK(lenslink_rig_select(&g_rig, g_view, "B"));

	snprintf(s[1].source_name, sizeof(s[1].source_name), "Stage Right");
	s[1].connected = false;
	s[1].tone = STATUS_TONE_WAIT;
	s[1].program = true;
	s[1].id = 99; /* even a different runtime id (source recreated) */
	build(s, 2, NULL);
	CHECK(find("B")->selected);
	CHECK_STR(find("B")->name, "Stage Right");
}

static void test_selection_when_camera_disappears(void)
{
	fresh();
	struct lenslink_source_info s[4] = {cam(1, "A", "a"), cam(2, "B", "b"),
					    cam(3, "C", "c"), cam(4, "D", "d")};
	build(s, 4, NULL);
	CHECK(lenslink_rig_select(&g_rig, g_view, "B"));
	build(s, 4, NULL);

	/* B is deleted: the selection moves to whoever now holds slot 2. */
	struct lenslink_source_info no_b[3] = {s[0], s[2], s[3]};
	build(no_b, 3, NULL);
	CHECK(find("C")->selected && !find("A")->selected);

	/* The last camera goes: falls to the new last, then to nothing. */
	CHECK(lenslink_rig_select(&g_rig, g_view, "D"));
	build(no_b, 3, NULL);
	struct lenslink_source_info only_ac[2] = {s[0], s[2]};
	build(only_ac, 2, NULL);
	CHECK(find("C")->selected);
	build(only_ac, 0, NULL);
	CHECK(g_view->count == 0);
	CHECK(g_rig.selected[0] == '\0');
}

static void test_scene_collection_reset(void)
{
	fresh();
	struct lenslink_source_info s[2] = {cam(1, "A", "a"), cam(2, "B", "b")};
	build(s, 2, NULL);
	CHECK(lenslink_rig_select(&g_rig, g_view, "B"));

	lenslink_rig_reset(&g_rig);
	CHECK(g_rig.selected[0] == '\0');
	/* The new collection has its own cameras and no memory of the old
	 * order: first-seen order, no stale selection. */
	struct lenslink_source_info other[2] = {cam(7, "Z", "z"),
						cam(8, "Y", "y")};
	build(other, 2, NULL);
	CHECK_STR(g_view->cards[0].uuid, "Z");
	CHECK(!find("Z")->selected && !find("Y")->selected);
	CHECK(find("A") == NULL);
}

static void test_sources_without_uuid_are_skipped(void)
{
	fresh();
	struct lenslink_source_info s[2] = {cam(1, "", "no uuid"),
					    cam(2, "B", "b")};
	build(s, 2, NULL);
	CHECK(g_view->count == 1);
	CHECK_STR(g_view->cards[0].uuid, "B");
}

/* --- health line text ------------------------------------------------ */

static void test_health_lines(void)
{
	fresh();
	struct lenslink_source_info h = cam(1, "A", "a");
	h.bitrate_bps = 11.2e6;
	h.latency_ms = 39;
	snprintf(h.transport, sizeof(h.transport), "USB");
	build(&h, 1, NULL);
	CHECK_STR(g_view->cards[0].health, "60 fps \xC2\xB7 11.2 Mb/s \xC2\xB7 39 ms \xC2\xB7 USB");

	h.latency_ms = 0; /* unknown */
	build(&h, 1, NULL);
	CHECK_STR(g_view->cards[0].health, "60 fps \xC2\xB7 11.2 Mb/s \xC2\xB7 USB");

	/* Standby, with its battery on the card's right. */
	struct lenslink_source_info sb = cam(2, "B", "b");
	sb.standby = true;
	sb.has_rates = false;
	snprintf(sb.transport, sizeof(sb.transport), "USB");
	sb.telemetry.has_battery_percent = true;
	sb.telemetry.battery_percent = 87;
	sb.telemetry.battery_state = LL_BATTERY_CHARGING;
	build(&sb, 1, NULL);
	CHECK_STR(g_view->cards[0].health, "Standby \xC2\xB7 USB");
	CHECK_STR(g_view->cards[0].battery, "87% \xE2\x9A\xA1");
	CHECK(g_view->cards[0].state == CARD_STANDBY);

	struct lenslink_source_info pz = cam(3, "C", "c");
	pz.paused = true;
	build(&pz, 1, NULL);
	CHECK_STR(g_view->cards[0].health, "Paused \xC2\xB7 Wi-Fi");
	CHECK(g_view->cards[0].state == CARD_PAUSED);

	struct lenslink_source_info cn = cam(4, "D", "d");
	cn.has_rates = false;
	build(&cn, 1, NULL);
	CHECK_STR(g_view->cards[0].health, "Connecting\xE2\x80\xA6 \xC2\xB7 Wi-Fi");
}

static void test_no_separator_artifacts(void)
{
	fresh();
	struct lenslink_source_info h = cam(1, "A", "a");
	h.transport[0] = '\0';
	h.latency_ms = 0;
	build(&h, 1, NULL);
	const char *line = g_view->cards[0].health;
	CHECK(strstr(line, "\xC2\xB7 \xC2\xB7") == NULL);
	CHECK(line[0] != ' ');
	size_t n = strlen(line);
	CHECK(n > 0 && line[n - 1] != ' ');
	CHECK_STR(line, "60 fps \xC2\xB7 10.4 Mb/s");
}

static void test_disconnected(void)
{
	fresh();
	struct lenslink_source_info h = cam(1, "A", "a");
	h.connected = false;
	h.tone = STATUS_TONE_WAIT;
	h.latency_ms = 41;
	/* The stale figures of a phone that went away. */
	h.telemetry.has_battery_percent = true;
	h.telemetry.battery_percent = 4;
	h.telemetry.system_pressure = LL_PRESSURE_CRITICAL;
	build(&h, 1, NULL);
	const struct lenslink_card *c = &g_view->cards[0];
	CHECK_STR(c->health, "Reconnecting\xE2\x80\xA6");
	CHECK_STR(c->battery, ""); /* no "4%" as though it were current */
	CHECK_STR(c->warning, ""); /* ...and no stale pressure warning */
	CHECK(c->state == CARD_OFFLINE);
	CHECK(c->attention == ATTENTION_WARNING); /* still counts */
	CHECK(g_view->summary.attention == 1);
	CHECK(g_view->summary.live == 0);
	CHECK_NEAR(g_view->summary.bitrate_bps, 0);
}

static void test_idle_shows_its_instruction(void)
{
	fresh();
	struct lenslink_source_info h = cam(1, "A", "a");
	h.connected = false;
	h.tone = STATUS_TONE_IDLE;
	snprintf(h.status, sizeof(h.status), "Enter the phone's IP address");
	build(&h, 1, NULL);
	CHECK(g_view->cards[0].state == CARD_IDLE);
	CHECK_STR(g_view->cards[0].health, "Enter the phone's IP address");
	CHECK(g_view->cards[0].attention == ATTENTION_NONE);
}

/* --- battery ---------------------------------------------------------- */

static void test_battery(void)
{
	fresh();
	struct lenslink_source_info h = cam(1, "A", "a");
	h.telemetry.has_battery_percent = true;
	h.telemetry.battery_percent = 81;
	h.telemetry.battery_state = LL_BATTERY_UNPLUGGED;
	build(&h, 1, NULL);
	CHECK_STR(g_view->cards[0].battery, "81%");
	CHECK(!g_view->cards[0].charging);

	h.telemetry.battery_state = LL_BATTERY_FULL;
	h.telemetry.battery_percent = 92;
	build(&h, 1, NULL);
	CHECK_STR(g_view->cards[0].battery, "92% \xE2\x9A\xA1");
	CHECK(g_view->cards[0].charging);

	/* Nothing known (an older app): nothing shown, no placeholder. */
	memset(&h.telemetry, 0, sizeof(h.telemetry));
	build(&h, 1, NULL);
	CHECK_STR(g_view->cards[0].battery, "");
	CHECK_STR(g_view->cards[0].warning, "");
}

static void test_low_battery_only_when_unplugged(void)
{
	fresh();
	struct lenslink_source_info h = cam(1, "A", "a");
	h.telemetry.has_battery_percent = true;
	h.telemetry.battery_percent = 12;
	h.telemetry.battery_state = LL_BATTERY_CHARGING;
	build(&h, 1, NULL);
	CHECK_STR(g_view->cards[0].warning, ""); /* plugged in: fine */
	CHECK(g_view->cards[0].attention == ATTENTION_NONE);

	h.telemetry.battery_state = LL_BATTERY_UNPLUGGED;
	build(&h, 1, NULL);
	CHECK_STR(g_view->cards[0].warning, "Low battery");
	CHECK(g_view->cards[0].battery_low);
	CHECK(g_view->cards[0].attention == ATTENTION_WARNING);

	h.telemetry.battery_state = LL_BATTERY_UNKNOWN; /* never "low" */
	build(&h, 1, NULL);
	CHECK_STR(g_view->cards[0].warning, "");
}

/* --- warnings --------------------------------------------------------- */

static void test_pressure_wording_follows_the_cause(void)
{
	fresh();
	struct lenslink_source_info h = cam(1, "A", "a");
	h.telemetry.system_pressure = LL_PRESSURE_SERIOUS;

	h.telemetry.system_pressure_factors =
		LL_PRESSURE_FACTOR_CAMERA_TEMPERATURE;
	build(&h, 1, NULL);
	CHECK_STR(g_view->cards[0].warning, "Camera thermal pressure");

	h.telemetry.system_pressure_factors = LL_PRESSURE_FACTOR_PEAK_POWER;
	build(&h, 1, NULL);
	CHECK_STR(g_view->cards[0].warning, "Camera power pressure");

	h.telemetry.system_pressure_factors = LL_PRESSURE_FACTOR_BATTERY_STRESS;
	build(&h, 1, NULL);
	CHECK_STR(g_view->cards[0].warning, "Camera power pressure");

	h.telemetry.system_pressure_factors =
		LL_PRESSURE_FACTOR_CAMERA_TEMPERATURE |
		LL_PRESSURE_FACTOR_PEAK_POWER;
	build(&h, 1, NULL);
	CHECK_STR(g_view->cards[0].warning,
		  "Camera thermal and power pressure");

	h.telemetry.system_pressure_factors = 0; /* elevated, unexplained */
	build(&h, 1, NULL);
	CHECK_STR(g_view->cards[0].warning, "Camera system pressure");
	CHECK(strstr(g_view->cards[0].warning, "thermal") == NULL);
}

static void test_device_thermal_is_not_camera_pressure(void)
{
	fresh();
	struct lenslink_source_info h = cam(1, "A", "a");
	h.telemetry.thermal_state = LL_THERMAL_SERIOUS;
	h.telemetry.system_pressure = LL_PRESSURE_NOMINAL;
	build(&h, 1, NULL);
	CHECK_STR(g_view->cards[0].warning, "Phone running hot");

	h.telemetry.thermal_state = LL_THERMAL_FAIR; /* not worth a line */
	build(&h, 1, NULL);
	CHECK_STR(g_view->cards[0].warning, "");
}

static void test_warning_priority(void)
{
	fresh();
	struct lenslink_source_info h = cam(1, "A", "a");
	struct lenslink_telemetry *t = &h.telemetry;
	t->has_low_power_mode = true;
	t->low_power_mode = true;
	t->has_battery_percent = true;
	t->battery_percent = 10;
	t->battery_state = LL_BATTERY_UNPLUGGED;
	t->thermal_state = LL_THERMAL_SERIOUS;
	t->system_pressure = LL_PRESSURE_SERIOUS;
	t->system_pressure_factors = LL_PRESSURE_FACTOR_CAMERA_TEMPERATURE;

	/* 7 (camera pressure) outranks 8 (device thermal), 9 (battery) and
	 * 10 (Low Power Mode). */
	build(&h, 1, NULL);
	CHECK_STR(g_view->cards[0].warning, "Camera thermal pressure");
	CHECK_STR(g_view->cards[0].note, ""); /* no quiet line beside it */

	t->system_pressure = LL_PRESSURE_NOMINAL; /* 8 next */
	build(&h, 1, NULL);
	CHECK_STR(g_view->cards[0].warning, "Phone running hot");

	t->thermal_state = LL_THERMAL_NOMINAL; /* 9 next */
	build(&h, 1, NULL);
	CHECK_STR(g_view->cards[0].warning, "Low battery");

	t->battery_state = LL_BATTERY_CHARGING; /* only LPM left */
	build(&h, 1, NULL);
	CHECK_STR(g_view->cards[0].warning, "");
	CHECK_STR(g_view->cards[0].note, "Low Power Mode");
	CHECK(g_view->cards[0].attention == ATTENTION_INFO);
	CHECK(g_view->summary.attention == 0); /* LPM alone is "healthy" */
	CHECK_STR(g_view->summary.status, "All healthy");

	/* Critical pressure (5) beats critical device heat (6) beats
	 * serious pressure (7). */
	t->system_pressure = LL_PRESSURE_CRITICAL;
	t->thermal_state = LL_THERMAL_CRITICAL;
	build(&h, 1, NULL);
	CHECK_STR(g_view->cards[0].warning,
		  "Camera thermal pressure \xC2\xB7 critical");
	CHECK(g_view->cards[0].attention == ATTENTION_CRITICAL);

	t->system_pressure = LL_PRESSURE_SERIOUS;
	build(&h, 1, NULL);
	CHECK_STR(g_view->cards[0].warning,
		  "Phone running hot \xC2\xB7 critical");

	/* Shutdown (4) beats everything telemetry has to say. */
	t->system_pressure = LL_PRESSURE_SHUTDOWN;
	build(&h, 1, NULL);
	CHECK_STR(g_view->cards[0].warning, "Camera shut down by iOS");
}

static void test_connection_warnings_outrank_telemetry(void)
{
	fresh();
	struct lenslink_source_info h = cam(1, "A", "a");
	h.telemetry.system_pressure = LL_PRESSURE_CRITICAL;

	h.connected = false;
	h.tone = STATUS_TONE_ERROR;
	h.identity = LENSLINK_IDENTITY_MISMATCH;
	build(&h, 1, NULL);
	CHECK(g_view->cards[0].state == CARD_WRONG_DEVICE);
	CHECK_STR(g_view->cards[0].warning, "Different phone answered");
	CHECK_STR(g_view->cards[0].health, "Reconnecting\xE2\x80\xA6");

	h.identity = LENSLINK_IDENTITY_UNIDENTIFIED;
	build(&h, 1, NULL);
	CHECK_STR(g_view->cards[0].warning, "Phone identity unconfirmed");

	h.identity = LENSLINK_IDENTITY_SEARCHING;
	build(&h, 1, NULL);
	CHECK(g_view->cards[0].state == CARD_ERROR);
	CHECK_STR(g_view->cards[0].warning, "Connection problem");

	/* Connected but the stream is in error. */
	h.connected = true;
	build(&h, 1, NULL);
	CHECK_STR(g_view->cards[0].warning, "Stream problem");
}

static void test_summary(void)
{
	fresh();
	struct lenslink_source_info s[4] = {cam(1, "A", "a"), cam(2, "B", "b"),
					    cam(3, "C", "c"), cam(4, "D", "d")};
	s[0].bitrate_bps = 11.2e6;
	s[1].bitrate_bps = 9.8e6;
	s[2].bitrate_bps = 10.4e6;
	s[3].connected = false;
	s[3].tone = STATUS_TONE_WAIT;
	s[3].bitrate_bps = 500e6; /* stale, must not count */
	build(s, 4, NULL);
	CHECK(g_view->summary.cameras == 4);
	CHECK(g_view->summary.live == 3);
	CHECK_NEAR(g_view->summary.bitrate_bps, 31.4e6);
	CHECK_STR(g_view->summary.headline,
		  "4 cameras \xC2\xB7 3 live \xC2\xB7 31.4 Mb/s");
	CHECK_STR(g_view->summary.status, "1 camera needs attention");

	/* Standby and paused are neither live nor problems. */
	s[3] = cam(4, "D", "d");
	s[3].standby = true;
	s[3].has_rates = false;
	s[2].paused = true;
	build(s, 4, NULL);
	CHECK(g_view->summary.live == 2);
	CHECK_STR(g_view->summary.status, "All healthy");
	CHECK_STR(g_view->summary.headline,
		  "4 cameras \xC2\xB7 2 live \xC2\xB7 21.0 Mb/s");

	s[0].telemetry.thermal_state = LL_THERMAL_SERIOUS;
	s[1].telemetry.thermal_state = LL_THERMAL_CRITICAL;
	build(s, 4, NULL);
	CHECK_STR(g_view->summary.status, "2 cameras need attention");
}

static void test_summary_edges(void)
{
	fresh();
	build(NULL, 0, NULL);
	CHECK_STR(g_view->summary.headline, "No cameras");
	CHECK_STR(g_view->summary.status, "");

	struct lenslink_source_info one = cam(1, "A", "a");
	one.connected = false;
	one.tone = STATUS_TONE_WAIT;
	build(&one, 1, NULL);
	CHECK_STR(g_view->summary.headline, "1 camera \xC2\xB7 0 live");

	/* A screen mirror never counts as a camera, live or otherwise. */
	struct lenslink_source_info s[2] = {screen(1, "S", "s"), cam(2, "A", "a")};
	build(s, 2, NULL);
	CHECK(g_view->summary.cameras == 1 && g_view->summary.live == 1);
}

static void test_actions(void)
{
	fresh();
	struct lenslink_source_info live = cam(1, "A", "a");
	build(&live, 1, NULL);
	CHECK(g_view->cards[0].can_pause && g_view->cards[0].can_stop &&
	      !g_view->cards[0].can_start && !g_view->cards[0].can_resume &&
	      g_view->cards[0].can_recalibrate);

	struct lenslink_source_info sb = cam(2, "B", "b");
	sb.standby = true;
	build(&sb, 1, NULL);
	CHECK(g_view->cards[0].can_start && !g_view->cards[0].can_stop &&
	      !g_view->cards[0].can_pause);

	struct lenslink_source_info pz = cam(3, "C", "c");
	pz.paused = true;
	build(&pz, 1, NULL);
	CHECK(g_view->cards[0].can_resume && g_view->cards[0].can_stop &&
	      !g_view->cards[0].can_pause);

	struct lenslink_source_info off = cam(4, "D", "d");
	off.connected = false;
	off.tone = STATUS_TONE_WAIT;
	build(&off, 1, NULL);
	CHECK(!g_view->cards[0].can_start && !g_view->cards[0].can_stop &&
	      !g_view->cards[0].can_pause && !g_view->cards[0].can_recalibrate);
}

static void test_text_fits_and_is_utf8_safe(void)
{
	fresh();
	struct lenslink_source_info h = cam(1, "A", "a");
	/* A name that fills its buffer with multi-byte characters. */
	for (size_t i = 0; i + 3 < sizeof(h.source_name); i += 3)
		memcpy(h.source_name + i, "\xE2\x9A\xA1", 3);
	snprintf(h.status, sizeof(h.status), "%s", h.source_name);
	h.telemetry.system_pressure = LL_PRESSURE_SHUTDOWN;
	build(&h, 1, NULL);
	const char *t = g_view->cards[0].tooltip;
	size_t n = strlen(t);
	CHECK(n < sizeof(g_view->cards[0].tooltip));
	/* Ends on a character boundary: no dangling half of a multi-byte
	 * character (a continuation byte with no lead byte before it). */
	size_t back = n;
	while (back > 0 && ((unsigned char)t[back - 1] & 0xC0) == 0x80)
		back--;
	CHECK(back > 0);
	unsigned char lead = (unsigned char)t[back - 1];
	size_t want = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
	CHECK(n - (back - 1) == want);
	CHECK(strlen(g_view->cards[0].accessible) <
	      sizeof(g_view->cards[0].accessible));
}

static void test_accessible_text_carries_state(void)
{
	fresh();
	struct lenslink_source_info s[2] = {cam(1, "A", "Stage Left"),
					    cam(2, "B", "Stage Right")};
	s[0].program = true;
	s[1].telemetry.has_battery_percent = true;
	s[1].telemetry.battery_percent = 81;
	s[1].telemetry.battery_state = LL_BATTERY_CHARGING;
	struct lenslink_uuid_set pv = {0};
	lenslink_uuid_set_add(&pv, "B");
	build(s, 2, &pv);
	CHECK(lenslink_rig_select(&g_rig, g_view, "B"));
	build(s, 2, &pv);

	CHECK(strstr(find("A")->accessible, "Camera 1") != NULL);
	CHECK(strstr(find("A")->accessible, "On air") != NULL);
	CHECK(strstr(find("A")->accessible, "In preview") == NULL);
	CHECK(strstr(find("B")->accessible, "In preview") != NULL);
	CHECK(strstr(find("B")->accessible, "Selected") != NULL);
	CHECK(strstr(find("B")->accessible, "charging") != NULL);
	CHECK(strstr(find("B")->accessible, "Battery 81%") != NULL);
}

int main(void)
{
	load_strings();
	g_view = calloc(1, sizeof(*g_view));

	test_tally_combinations();
	test_selecting_touches_no_tally();
	test_program_and_preview_at_once();
	test_showing_somewhere_is_not_preview();
	test_program_is_libobs_active_state();
	test_numbers_follow_slots_and_never_reshuffle();
	test_saved_order_and_new_camera();
	test_screens_are_not_cameras();
	test_selection_survives_rename_and_status();
	test_selection_when_camera_disappears();
	test_scene_collection_reset();
	test_sources_without_uuid_are_skipped();
	test_health_lines();
	test_no_separator_artifacts();
	test_disconnected();
	test_idle_shows_its_instruction();
	test_battery();
	test_low_battery_only_when_unplugged();
	test_pressure_wording_follows_the_cause();
	test_device_thermal_is_not_camera_pressure();
	test_warning_priority();
	test_connection_warnings_outrank_telemetry();
	test_summary();
	test_summary_edges();
	test_actions();
	test_text_fits_and_is_utf8_safe();
	test_accessible_text_carries_state();

	CHECK(g_missing == 0);
	free(g_view);
	TEST_MAIN_END();
}
