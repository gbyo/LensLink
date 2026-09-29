#include "camera-state.h"
#include "test.h"

static bool parse(const char *json, struct lenslink_camera_state *st)
{
	return lenslink_camera_state_parse(json, strlen(json), st);
}

/* Shaped like Streamer.swift's snapshot (JSONSerialization output). */
static const char *const full_snapshot =
	"{\"paused\":false,\"pauseReason\":\"\",\"zoom\":2.5,\"maxZoom\":10,"
	"\"exposureBias\":-0.5,\"focusMode\":\"locked\",\"lensPosition\":0.4,"
	"\"faceFocus\":true,\"supportsFaceFocus\":true,\"flashlight\":false,"
	"\"hasFlashlight\":true,\"camera\":\"back\","
	"\"lens\":\"Main (Wide)\","
	"\"lenses\":[\"Ultra Wide (0.5\xC3\x97)\",\"Main (Wide)\","
	"\"Telephoto\",\"Front\"],"
	"\"whiteBalanceMode\":\"locked\",\"whiteBalanceTemperature\":4800,"
	"\"supportsWhiteBalanceLock\":true,\"exposureMode\":\"manual\","
	"\"iso\":400,\"minISO\":34,\"maxISO\":3072,"
	"\"shutterSeconds\":0.016666666666666666,"
	"\"minShutterSeconds\":0.000125,\"maxShutterSeconds\":0.0333,"
	"\"supportsManualExposure\":true,\"resolution\":\"1080p\",\"fps\":60,"
	"\"codec\":\"hevc\",\"resolutions\":[\"720p\",\"1080p\",\"4k\"],"
	"\"frameRates\":[24,30,60],\"codecs\":[\"h264\",\"hevc\"],"
	"\"quality\":\"maximum\",\"supportsGreenScreen\":true,"
	"\"greenScreen\":true,\"greenScreenDepth\":true,"
	"\"greenScreenMaxDistance\":1.5,\"micEnabled\":true,"
	"\"mic\":\"builtin:1835\",\"mics\":[{\"id\":\"auto\",\"name\":\"Automatic\"},"
	"{\"id\":\"builtin:1835\",\"name\":\"Bottom\"},"
	"{\"id\":\"port:AA:BB\\/tacl\",\"name\":\"AirPods \\\"Pro\\\"\"}]}";

static void test_full_snapshot(void)
{
	struct lenslink_camera_state st;
	CHECK(parse(full_snapshot, &st));
	CHECK(st.valid);

	CHECK(!st.paused && st.pause_reason == LL_PAUSE_NONE);
	CHECK(st.has & LLCS_HAS_ZOOM);
	CHECK_NEAR(st.zoom, 2.5);
	CHECK_NEAR(st.max_zoom, 10.0);
	CHECK_NEAR(st.exposure_bias, -0.5);
	CHECK(st.focus_mode == LL_FOCUS_LOCKED);
	CHECK_NEAR(st.lens_position, 0.4);
	CHECK(st.face_focus && st.supports_face_focus);
	CHECK(!st.flashlight && st.has_flashlight);
	CHECK(st.camera == LL_CAMERA_BACK);
	CHECK_STR(st.lens, "Main (Wide)");
	CHECK(st.lens_count == 4);
	CHECK_STR(st.lenses[0], "Ultra Wide (0.5\xC3\x97)");
	CHECK_STR(st.lenses[3], "Front");

	CHECK(st.wb_mode == LL_WB_LOCKED);
	CHECK_NEAR(st.wb_temperature, 4800.0);
	CHECK(st.supports_wb_lock);
	CHECK(st.exposure_mode == LL_EXPOSURE_MANUAL);
	CHECK_NEAR(st.iso, 400.0);
	CHECK(st.has & LLCS_HAS_ISO_RANGE);
	CHECK_NEAR(st.min_iso, 34.0);
	CHECK_NEAR(st.max_iso, 3072.0);
	CHECK_NEAR(st.shutter_seconds, 1.0 / 60.0);
	CHECK(st.has & LLCS_HAS_SHUTTER_RANGE);
	CHECK_NEAR(st.min_shutter_seconds, 0.000125);
	CHECK(st.supports_manual_exposure);

	CHECK_STR(st.resolution, "1080p");
	CHECK(st.fps == 60 && (st.has & LLCS_HAS_FPS));
	CHECK_STR(st.codec, "hevc");
	CHECK_STR(st.quality, "maximum");
	CHECK(st.resolution_count == 3);
	CHECK_STR(st.resolutions[2], "4k");
	CHECK(st.frame_rate_count == 3 && st.frame_rates[2] == 60);
	CHECK(st.codec_count == 2);
	CHECK_STR(st.codecs[1], "hevc");

	CHECK(!st.hdr && !(st.has & LLCS_HAS_COLOR));
	CHECK(!lenslink_camera_state_ten_bit(&st));

	CHECK(st.supports_green_screen && st.green_screen &&
	      st.green_screen_depth);
	CHECK_NEAR(st.green_screen_max_distance, 1.5);

	CHECK(st.mic_enabled);
	CHECK_STR(st.mic, "builtin:1835");
	CHECK(st.mic_count == 3);
	CHECK_STR(st.mics[2].id, "port:AA:BB/tacl");
	CHECK_STR(st.mics[2].name, "AirPods \"Pro\"");
}

/* An old app: the PROTOCOL.md §8 example, nothing newer. */
static void test_old_app(void)
{
	struct lenslink_camera_state st;
	CHECK(parse("{ \"zoom\": 2.5, \"maxZoom\": 10, \"exposureBias\": -0.5,"
		    "  \"focusMode\": \"locked\", \"lensPosition\": 0.4,"
		    "  \"flashlight\": true, \"hasFlashlight\": true,"
		    "  \"camera\": \"back\" }",
		    &st));
	CHECK(st.valid);
	CHECK(st.flashlight);
	CHECK(st.exposure_mode == LL_EXPOSURE_UNKNOWN);
	CHECK(st.wb_mode == LL_WB_UNKNOWN);
	CHECK(!(st.has & LLCS_HAS_ISO));
	CHECK(!(st.has & LLCS_HAS_FPS));
	CHECK(st.lens_count == 0 && st.codec_count == 0 && st.mic_count == 0);
	CHECK(!st.supports_green_screen && !st.paused);
	CHECK(st.resolution[0] == 0);
}

static void test_empty_and_unknown(void)
{
	struct lenslink_camera_state st;
	CHECK(parse("{}", &st) && st.valid && st.has == 0);

	/* Future keys (and nested shapes) are skipped, not fatal. */
	CHECK(parse("{\"battery\":83,\"charging\":true,"
		    "\"future\":{\"nested\":[1,{\"x\":null}]},\"zoom\":3}",
		    &st));
	CHECK(st.valid);
	CHECK_NEAR(st.zoom, 3.0);
}

static void test_pause_and_color(void)
{
	struct lenslink_camera_state st;
	CHECK(parse("{\"paused\":true,\"pauseReason\":\"camera\"}", &st));
	CHECK(st.paused && st.pause_reason == LL_PAUSE_CAMERA);
	CHECK(parse("{\"paused\":true,\"pauseReason\":\"user\"}", &st));
	CHECK(st.pause_reason == LL_PAUSE_USER);
	CHECK(parse("{\"paused\":true,\"pauseReason\":\"thermal\"}", &st));
	CHECK(st.pause_reason == LL_PAUSE_OTHER);
	/* A reason without the pause means nothing. */
	CHECK(parse("{\"paused\":false,\"pauseReason\":\"user\"}", &st));
	CHECK(st.pause_reason == LL_PAUSE_NONE);

	CHECK(parse("{\"hdr\":true}", &st) &&
	      lenslink_camera_state_ten_bit(&st));
	CHECK(parse("{\"color\":\"log\"}", &st) &&
	      lenslink_camera_state_ten_bit(&st));
	CHECK_STR(st.color, "log");
	/* Any colour key means 10-bit, as the old strstr check did. */
	CHECK(parse("{\"color\":7}", &st) &&
	      lenslink_camera_state_ten_bit(&st));
	CHECK(parse("{\"hdr\":false}", &st) &&
	      !lenslink_camera_state_ten_bit(&st));
}

/* Values of the wrong type are unknown; the rest of the snapshot still
 * parses. */
static void test_wrong_types(void)
{
	struct lenslink_camera_state st;
	CHECK(parse("{\"zoom\":\"2\",\"flashlight\":1,\"fps\":\"60\","
		    "\"lenses\":\"Main\",\"frameRates\":[30,\"60\",null,24],"
		    "\"focusMode\":true,\"mics\":[1,{\"name\":\"no id\"},"
		    "{\"id\":\"auto\"}],\"iso\":200}",
		    &st));
	CHECK(st.valid);
	CHECK(!(st.has & LLCS_HAS_ZOOM));
	CHECK(!st.flashlight);
	CHECK(!(st.has & LLCS_HAS_FPS));
	CHECK(st.lens_count == 0);
	CHECK(st.frame_rate_count == 2 && st.frame_rates[0] == 30 &&
	      st.frame_rates[1] == 24);
	CHECK(st.focus_mode == LL_FOCUS_UNKNOWN);
	CHECK(st.mic_count == 1);
	CHECK_STR(st.mics[0].id, "auto");
	CHECK_NEAR(st.iso, 200.0);

	/* A lone half of a range is not a range. */
	CHECK(parse("{\"minISO\":34}", &st));
	CHECK(!(st.has & LLCS_HAS_ISO_RANGE));
	CHECK(parse("{\"minISO\":\"34\",\"maxISO\":3072}", &st));
	CHECK(!(st.has & LLCS_HAS_ISO_RANGE));
	/* Swapped halves are put in order. */
	CHECK(parse("{\"minISO\":3072,\"maxISO\":34}", &st));
	CHECK((st.has & LLCS_HAS_ISO_RANGE) && st.min_iso < st.max_iso);
}

static void test_clamping(void)
{
	struct lenslink_camera_state st;
	CHECK(parse("{\"zoom\":1e308,\"lensPosition\":-3,\"exposureBias\":99,"
		    "\"whiteBalanceTemperature\":0,\"fps\":1e9,"
		    "\"frameRates\":[-5,100000],\"iso\":1e999}",
		    &st));
	CHECK(st.zoom <= 1000.0);
	CHECK(st.lens_position == 0.0);
	CHECK(st.exposure_bias <= 16.0);
	CHECK(st.wb_temperature >= 1000.0);
	CHECK(st.fps <= 1000);
	CHECK(st.frame_rates[0] == 0 && st.frame_rates[1] <= 1000);
	/* inf is non-finite: unknown, not clamped. */
	CHECK(!(st.has & LLCS_HAS_ISO));
}

static void test_capacity(void)
{
	struct lenslink_camera_state st;
	/* More list items than the tables hold: extras dropped. */
	CHECK(parse("{\"lenses\":[\"1\",\"2\",\"3\",\"4\",\"5\",\"6\",\"7\","
		    "\"8\",\"9\",\"10\"],\"codecs\":[\"a\",\"b\",\"c\",\"d\","
		    "\"e\"]}",
		    &st));
	CHECK(st.lens_count == LLCS_MAX_LENSES);
	CHECK_STR(st.lenses[7], "8");
	CHECK(st.codec_count == LLCS_MAX_CODECS);

	/* An over-long mic id can't be selected: dropped rather than kept
	 * truncated. */
	char json[512];
	char id[200];
	memset(id, 'x', sizeof(id) - 1);
	id[sizeof(id) - 1] = 0;
	snprintf(json, sizeof(json),
		 "{\"mics\":[{\"id\":\"%s\",\"name\":\"Long\"},"
		 "{\"id\":\"auto\",\"name\":\"Automatic\"}]}",
		 id);
	CHECK(parse(json, &st));
	CHECK(st.mic_count == 1);
	CHECK_STR(st.mics[0].id, "auto");

	/* An over-long lens label is truncated on a code point boundary. */
	char lens[128];
	snprintf(lens, sizeof(lens),
		 "{\"lens\":\"%s\"}",
		 "\xC3\x97\xC3\x97\xC3\x97\xC3\x97\xC3\x97\xC3\x97\xC3\x97"
		 "\xC3\x97\xC3\x97\xC3\x97\xC3\x97\xC3\x97\xC3\x97\xC3\x97"
		 "\xC3\x97\xC3\x97\xC3\x97\xC3\x97\xC3\x97\xC3\x97\xC3\x97"
		 "\xC3\x97\xC3\x97\xC3\x97\xC3\x97");
	CHECK(parse(lens, &st));
	CHECK(strlen(st.lens) == 46); /* 23 whole ×, not 23.5 */
}

static void test_malformed(void)
{
	struct lenslink_camera_state st;
	static const char *const bad[] = {
		"",
		"[]",
		"\"state\"",
		"null",
		"{\"zoom\":2.5",
		"{\"zoom\":2.5,}",
		"{\"zoom\":}",
		"{\"lens\":\"unterminated}",
		"{\"zoom\":2.5}{\"zoom\":3}",
		"{\"lenses\":[\"a\",]}",
		"{zoom:1}",
	};
	for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
		memset(&st, 0xAB, sizeof(st));
		CHECK(!parse(bad[i], &st));
		/* Fully cleared: no half-filled state leaks out. */
		CHECK(!st.valid && st.has == 0 && st.lens_count == 0 &&
		      st.zoom == 0.0);
	}

	/* Every truncation of the real snapshot is rejected cleanly. */
	size_t len = strlen(full_snapshot);
	for (size_t n = 0; n < len; n++)
		CHECK(!lenslink_camera_state_parse(full_snapshot, n, &st));
	CHECK(lenslink_camera_state_parse(full_snapshot, len, &st));
}


/* ---- Telemetry ---------------------------------------------------------- */

/* A Stage 3 snapshot: the full Stage 2 one plus a serious, camera-thermal
 * pressure episode. */
static const char *const telemetry_snapshot =
	"{\"paused\":false,\"zoom\":2.5,\"fps\":60,\"codec\":\"hevc\","
	"\"batteryPercent\":87,\"batteryState\":\"charging\","
	"\"lowPowerMode\":false,\"thermalState\":\"fair\","
	"\"systemPressure\":\"serious\","
	"\"systemPressureFactors\":[\"cameraTemperature\","
	"\"systemTemperature\"]}";

static bool parse_telemetry(const char *fields, struct lenslink_telemetry *t)
{
	char json[512];
	struct lenslink_camera_state st;
	snprintf(json, sizeof(json), "{%s}", fields);
	bool ok = parse(json, &st);
	*t = st.telemetry;
	return ok && st.valid;
}

static void test_telemetry_full(void)
{
	struct lenslink_camera_state st;
	CHECK(parse(telemetry_snapshot, &st));
	const struct lenslink_telemetry *t = &st.telemetry;
	CHECK(t->has_battery_percent && t->battery_percent == 87);
	CHECK(t->battery_state == LL_BATTERY_CHARGING);
	CHECK(t->has_low_power_mode && !t->low_power_mode);
	CHECK(t->thermal_state == LL_THERMAL_FAIR);
	CHECK(t->system_pressure == LL_PRESSURE_SERIOUS);
	CHECK(t->system_pressure_factors ==
	      (LL_PRESSURE_FACTOR_CAMERA_TEMPERATURE |
	       LL_PRESSURE_FACTOR_SYSTEM_TEMPERATURE));
	/* The rest of the snapshot is untouched by the new keys. */
	CHECK_NEAR(st.zoom, 2.5);
	CHECK(st.fps == 60);
	CHECK_STR(st.codec, "hevc");
}

/* An app without telemetry: every fact unknown, nothing fabricated, and
 * everything else parses exactly as before. */
static void test_telemetry_old_app(void)
{
	struct lenslink_camera_state old, with;
	char json[4096];
	CHECK(parse(full_snapshot, &old));
	const struct lenslink_telemetry none = {0};
	CHECK(memcmp(&old.telemetry, &none, sizeof(none)) == 0);
	CHECK(!lenslink_telemetry_any(&old.telemetry));
	CHECK(lenslink_telemetry_severity(&old.telemetry) == LL_SEVERITY_NORMAL);
	CHECK(lenslink_telemetry_pressure_cause(&old.telemetry) ==
	      LL_PRESSURE_CAUSE_NONE);

	/* Stage 2's snapshot + the new keys == Stage 3's: same struct once
	 * the telemetry is set aside. */
	size_t n = strlen(full_snapshot);
	memcpy(json, full_snapshot, n - 1); /* drop the closing brace */
	snprintf(json + n - 1, sizeof(json) - n + 1,
		 ",\"batteryPercent\":19,\"batteryState\":\"unplugged\","
		 "\"lowPowerMode\":true,\"thermalState\":\"serious\","
		 "\"systemPressure\":\"critical\","
		 "\"systemPressureFactors\":[\"peakPower\"]}");
	CHECK(parse(json, &with));
	CHECK(lenslink_telemetry_any(&with.telemetry));
	with.telemetry = old.telemetry;
	CHECK(memcmp(&old, &with, sizeof(old)) == 0);
}

static void test_telemetry_battery_percent(void)
{
	struct lenslink_telemetry t;
	static const struct {
		const char *json;
		int expect; /* -1 = unknown */
	} cases[] = {
		{"0", 0},          {"1", 1},          {"20", 20},
		{"21", 21},        {"99", 99},        {"100", 100},
		{"87.0", 87},      {"8.7e1", 87},     {"-0", 0},
		/* Impossible values are unknown, never clamped. */
		{"-1", -1},        {"101", -1},       {"140", -1},
		{"1e308", -1},     {"1e999", -1},     {"-1e999", -1},
		{"100.5", -1},     {"87.5", -1},      {"0.5", -1},
		{"\"87\"", -1},    {"null", -1},      {"true", -1},
		{"[87]", -1},      {"{\"v\":87}", -1},
	};
	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		char fields[64];
		snprintf(fields, sizeof(fields), "\"batteryPercent\":%s",
			 cases[i].json);
		CHECK(parse_telemetry(fields, &t));
		CHECK(t.has_battery_percent == (cases[i].expect >= 0));
		CHECK(t.battery_percent ==
		      (cases[i].expect >= 0 ? cases[i].expect : 0));
	}
	/* Absent: unknown. */
	CHECK(parse_telemetry("\"zoom\":2", &t) && !t.has_battery_percent);

	/* A later malformed duplicate must not leave the earlier value
	 * trusted; a later good one wins. */
	CHECK(parse_telemetry("\"batteryPercent\":87,\"batteryPercent\":\"x\"",
			      &t) &&
	      !t.has_battery_percent && t.battery_percent == 0);
	CHECK(parse_telemetry("\"batteryPercent\":140,\"batteryPercent\":30",
			      &t) &&
	      t.has_battery_percent && t.battery_percent == 30);
	CHECK(parse_telemetry("\"batteryPercent\":30,\"batteryPercent\":140",
			      &t) &&
	      !t.has_battery_percent);
}

static void test_telemetry_battery_state(void)
{
	struct lenslink_telemetry t;
	CHECK(parse_telemetry("\"batteryState\":\"unplugged\"", &t) &&
	      t.battery_state == LL_BATTERY_UNPLUGGED);
	CHECK(parse_telemetry("\"batteryState\":\"charging\"", &t) &&
	      t.battery_state == LL_BATTERY_CHARGING);
	CHECK(parse_telemetry("\"batteryState\":\"full\"", &t) &&
	      t.battery_state == LL_BATTERY_FULL);
	CHECK(parse_telemetry("\"batteryState\":\"unknown\"", &t) &&
	      t.battery_state == LL_BATTERY_UNKNOWN);
	/* Future strings, case variants, junk and the wrong type: unknown. */
	static const char *const bad[] = {
		"\"wireless\"", "\"Charging\"", "\"\"",  "5",
		"null",         "true",         "[]",    "{}",
		"\"chargingchargingchargingchargingcharging\"",
	};
	for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
		char fields[128];
		snprintf(fields, sizeof(fields), "\"batteryState\":%s", bad[i]);
		CHECK(parse_telemetry(fields, &t));
		CHECK(t.battery_state == LL_BATTERY_UNKNOWN);
	}
	CHECK(parse_telemetry("\"batteryState\":\"full\","
			      "\"batteryState\":7",
			      &t) &&
	      t.battery_state == LL_BATTERY_UNKNOWN);
}

static void test_telemetry_low_power_mode(void)
{
	struct lenslink_telemetry t;
	CHECK(parse_telemetry("\"lowPowerMode\":true", &t) &&
	      t.has_low_power_mode && t.low_power_mode);
	CHECK(parse_telemetry("\"lowPowerMode\":false", &t) &&
	      t.has_low_power_mode && !t.low_power_mode);
	/* Missing and wrong type both mean unknown — not "off". */
	CHECK(parse_telemetry("\"zoom\":1", &t) && !t.has_low_power_mode &&
	      !t.low_power_mode);
	static const char *const bad[] = {"1", "0", "\"true\"", "null", "[]"};
	for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
		char fields[64];
		snprintf(fields, sizeof(fields), "\"lowPowerMode\":%s", bad[i]);
		CHECK(parse_telemetry(fields, &t));
		CHECK(!t.has_low_power_mode && !t.low_power_mode);
	}
	CHECK(parse_telemetry("\"lowPowerMode\":true,\"lowPowerMode\":1", &t) &&
	      !t.has_low_power_mode);
	/* Low Power Mode is not inferred from the battery, nor the reverse. */
	CHECK(parse_telemetry("\"batteryPercent\":5,\"batteryState\":"
			      "\"unplugged\"",
			      &t) &&
	      !t.has_low_power_mode);
	CHECK(parse_telemetry("\"lowPowerMode\":true", &t) &&
	      !t.has_battery_percent);
}

static void test_telemetry_thermal_and_pressure(void)
{
	struct lenslink_telemetry t;
	static const char *const thermal[] = {"nominal", "fair", "serious",
					      "critical"};
	static const enum lenslink_thermal_state thermal_v[] = {
		LL_THERMAL_NOMINAL, LL_THERMAL_FAIR, LL_THERMAL_SERIOUS,
		LL_THERMAL_CRITICAL};
	for (size_t i = 0; i < 4; i++) {
		char f[64];
		snprintf(f, sizeof(f), "\"thermalState\":\"%s\"", thermal[i]);
		CHECK(parse_telemetry(f, &t) && t.thermal_state == thermal_v[i]);
		CHECK_STR(lenslink_thermal_state_name(thermal_v[i]), thermal[i]);
	}
	static const char *const pressure[] = {"nominal", "fair", "serious",
					       "critical", "shutdown"};
	static const enum lenslink_system_pressure pressure_v[] = {
		LL_PRESSURE_NOMINAL, LL_PRESSURE_FAIR, LL_PRESSURE_SERIOUS,
		LL_PRESSURE_CRITICAL, LL_PRESSURE_SHUTDOWN};
	for (size_t i = 0; i < 5; i++) {
		char f[64];
		snprintf(f, sizeof(f), "\"systemPressure\":\"%s\"",
			 pressure[i]);
		CHECK(parse_telemetry(f, &t) &&
		      t.system_pressure == pressure_v[i]);
		CHECK_STR(lenslink_system_pressure_name(pressure_v[i]),
			  pressure[i]);
	}

	/* The two are independent facts: neither implies the other. */
	CHECK(parse_telemetry("\"systemPressure\":\"serious\"", &t) &&
	      t.thermal_state == LL_THERMAL_UNKNOWN);
	CHECK(parse_telemetry("\"thermalState\":\"serious\"", &t) &&
	      t.system_pressure == LL_PRESSURE_UNKNOWN);
	/* "shutdown" is a pressure level, not a thermal state, and
	 * "critical"/"fair" mean different things in each. */
	CHECK(parse_telemetry("\"thermalState\":\"shutdown\"", &t) &&
	      t.thermal_state == LL_THERMAL_UNKNOWN);

	static const char *const bad[] = {"\"hot\"", "\"Nominal\"", "\"\"", "1",
					  "null", "false", "[\"serious\"]"};
	for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
		char f[96];
		snprintf(f, sizeof(f), "\"thermalState\":%s,\"systemPressure\":%s",
			 bad[i], bad[i]);
		CHECK(parse_telemetry(f, &t));
		CHECK(t.thermal_state == LL_THERMAL_UNKNOWN &&
		      t.system_pressure == LL_PRESSURE_UNKNOWN);
	}
	CHECK(parse_telemetry("\"thermalState\":\"serious\","
			      "\"thermalState\":\"lava\"",
			      &t) &&
	      t.thermal_state == LL_THERMAL_UNKNOWN);
}

static void test_telemetry_factors(void)
{
	struct lenslink_telemetry t;
	const char *const p = "\"systemPressure\":\"serious\",";
	char f[512];

#define FACTORS(list)                                                     \
	do {                                                              \
		snprintf(f, sizeof(f), "%s\"systemPressureFactors\":%s", p, \
			 list);                                           \
		CHECK(parse_telemetry(f, &t));                            \
	} while (0)

	FACTORS("[]");
	CHECK(t.system_pressure_factors == 0);
	FACTORS("[\"peakPower\"]");
	CHECK(t.system_pressure_factors == LL_PRESSURE_FACTOR_PEAK_POWER);
	FACTORS("[\"systemTemperature\",\"cameraTemperature\","
		"\"depthModuleTemperature\",\"peakPower\",\"batteryStress\"]");
	CHECK(t.system_pressure_factors ==
	      (LL_PRESSURE_FACTOR_SYSTEM_TEMPERATURE |
	       LL_PRESSURE_FACTOR_CAMERA_TEMPERATURE |
	       LL_PRESSURE_FACTOR_DEPTH_MODULE_TEMPERATURE |
	       LL_PRESSURE_FACTOR_PEAK_POWER |
	       LL_PRESSURE_FACTOR_BATTERY_STRESS));
	/* Repeats change nothing. */
	FACTORS("[\"peakPower\",\"peakPower\",\"peakPower\"]");
	CHECK(t.system_pressure_factors == LL_PRESSURE_FACTOR_PEAK_POWER);
	/* A factor from the future costs nothing else. */
	FACTORS("[\"quantumFlux\"]");
	CHECK(t.system_pressure_factors == 0);
	FACTORS("[\"quantumFlux\",\"cameraTemperature\",\"\","
		"\"CAMERATEMPERATURE\"]");
	CHECK(t.system_pressure_factors ==
	      LL_PRESSURE_FACTOR_CAMERA_TEMPERATURE);
	/* Non-string entries are skipped; a non-array is nothing at all. */
	FACTORS("[1,null,{\"a\":[]},\"peakPower\",[\"cameraTemperature\"]]");
	CHECK(t.system_pressure_factors == LL_PRESSURE_FACTOR_PEAK_POWER);
	FACTORS("\"peakPower\"");
	CHECK(t.system_pressure_factors == 0);
	FACTORS("{\"peakPower\":true}");
	CHECK(t.system_pressure_factors == 0);
	FACTORS("null");
	CHECK(t.system_pressure_factors == 0);
	/* An over-long name is not a prefix match. */
	FACTORS("[\"cameraTemperatureXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX\"]");
	CHECK(t.system_pressure_factors == 0);
	/* Last key wins, even when malformed. */
	snprintf(f, sizeof(f),
		 "%s\"systemPressureFactors\":[\"peakPower\"],"
		 "\"systemPressureFactors\":7",
		 p);
	CHECK(parse_telemetry(f, &t) && t.system_pressure_factors == 0);
#undef FACTORS

	/* An oversized array (parsed in full, bounded by the payload). */
	static char big[20000];
	size_t n = 0;
	n += (size_t)snprintf(big + n, sizeof(big) - n,
			      "{\"systemPressure\":\"critical\","
			      "\"systemPressureFactors\":[");
	for (int i = 0; i < 1200; i++)
		n += (size_t)snprintf(big + n, sizeof(big) - n, "%s\"f%d\"",
				      i ? "," : "", i);
	n += (size_t)snprintf(big + n, sizeof(big) - n,
			      ",\"peakPower\"]}");
	struct lenslink_camera_state st;
	CHECK(lenslink_camera_state_parse(big, n, &st));
	CHECK(st.telemetry.system_pressure == LL_PRESSURE_CRITICAL);
	CHECK(st.telemetry.system_pressure_factors ==
	      LL_PRESSURE_FACTOR_PEAK_POWER);

	/* Factors explain a level; with no level they are a fragment of
	 * something malformed. Order in the object doesn't matter. */
	CHECK(parse_telemetry("\"systemPressureFactors\":[\"peakPower\"]", &t) &&
	      t.system_pressure_factors == 0);
	CHECK(parse_telemetry("\"systemPressureFactors\":[\"peakPower\"],"
			      "\"systemPressure\":\"fair\"",
			      &t) &&
	      t.system_pressure_factors == LL_PRESSURE_FACTOR_PEAK_POWER);
	CHECK(parse_telemetry("\"systemPressureFactors\":[\"peakPower\"],"
			      "\"systemPressure\":\"bogus\"",
			      &t) &&
	      t.system_pressure_factors == 0);
}

static struct lenslink_telemetry T(int percent, enum lenslink_battery_state b,
				   int lpm, enum lenslink_thermal_state th,
				   enum lenslink_system_pressure sp,
				   uint32_t factors)
{
	struct lenslink_telemetry t = {0};
	if (percent >= 0) {
		t.has_battery_percent = true;
		t.battery_percent = percent;
	}
	t.battery_state = b;
	if (lpm >= 0) {
		t.has_low_power_mode = true;
		t.low_power_mode = lpm != 0;
	}
	t.thermal_state = th;
	t.system_pressure = sp;
	t.system_pressure_factors = factors;
	return t;
}

/* The operator semantics: which facts are worth interrupting for, and
 * what caused an elevated camera pressure. */
static void test_telemetry_derivations(void)
{
	struct lenslink_telemetry t;
	enum { CAM = LL_PRESSURE_FACTOR_CAMERA_TEMPERATURE,
	       SYS = LL_PRESSURE_FACTOR_SYSTEM_TEMPERATURE,
	       DEPTH = LL_PRESSURE_FACTOR_DEPTH_MODULE_TEMPERATURE,
	       PEAK = LL_PRESSURE_FACTOR_PEAK_POWER,
	       BSTRESS = LL_PRESSURE_FACTOR_BATTERY_STRESS };

	/* Quiet: healthy, and also "we know nothing". */
	t = T(87, LL_BATTERY_CHARGING, 0, LL_THERMAL_NOMINAL,
	      LL_PRESSURE_NOMINAL, 0);
	CHECK(lenslink_telemetry_severity(&t) == LL_SEVERITY_NORMAL);
	t = T(50, LL_BATTERY_UNPLUGGED, 0, LL_THERMAL_FAIR, LL_PRESSURE_FAIR, 0);
	CHECK(lenslink_telemetry_severity(&t) == LL_SEVERITY_NORMAL);
	CHECK(lenslink_telemetry_pressure_cause(&t) == LL_PRESSURE_CAUSE_NONE);
	memset(&t, 0, sizeof(t));
	CHECK(lenslink_telemetry_severity(&t) == LL_SEVERITY_NORMAL);

	/* The 20% edge, unplugged only — and not a Low Power Mode. */
	t = T(21, LL_BATTERY_UNPLUGGED, 0, 0, 0, 0);
	CHECK(!lenslink_telemetry_battery_low(&t));
	t = T(20, LL_BATTERY_UNPLUGGED, 0, 0, 0, 0);
	CHECK(lenslink_telemetry_battery_low(&t));
	CHECK(lenslink_telemetry_severity(&t) == LL_SEVERITY_WARNING);
	t = T(19, LL_BATTERY_UNPLUGGED, 0, 0, 0, 0);
	CHECK(lenslink_telemetry_severity(&t) == LL_SEVERITY_WARNING);
	t = T(19, LL_BATTERY_CHARGING, 0, 0, 0, 0);
	CHECK(!lenslink_telemetry_battery_low(&t) &&
	      lenslink_telemetry_severity(&t) == LL_SEVERITY_NORMAL);
	t = T(3, LL_BATTERY_FULL, 0, 0, 0, 0);
	CHECK(!lenslink_telemetry_battery_low(&t));
	t = T(3, LL_BATTERY_UNKNOWN, 0, 0, 0, 0);
	CHECK(!lenslink_telemetry_battery_low(&t));
	t = T(-1, LL_BATTERY_UNPLUGGED, 0, 0, 0, 0);
	CHECK(!lenslink_telemetry_battery_low(&t));

	/* Low Power Mode is information, however full the battery. */
	t = T(74, LL_BATTERY_UNPLUGGED, 1, LL_THERMAL_NOMINAL,
	      LL_PRESSURE_NOMINAL, 0);
	CHECK(!lenslink_telemetry_battery_low(&t));
	CHECK(lenslink_telemetry_severity(&t) == LL_SEVERITY_INFO);
	/* ...and never masks something worse. */
	t = T(15, LL_BATTERY_UNPLUGGED, 1, 0, 0, 0);
	CHECK(lenslink_telemetry_severity(&t) == LL_SEVERITY_WARNING);

	/* Whole-device thermal state. */
	t = T(90, LL_BATTERY_CHARGING, 0, LL_THERMAL_SERIOUS,
	      LL_PRESSURE_NOMINAL, 0);
	CHECK(lenslink_telemetry_severity(&t) == LL_SEVERITY_WARNING);
	t = T(90, LL_BATTERY_CHARGING, 0, LL_THERMAL_CRITICAL,
	      LL_PRESSURE_NOMINAL, 0);
	CHECK(lenslink_telemetry_severity(&t) == LL_SEVERITY_CRITICAL);

	/* Camera pressure: serious is a warning, critical/shutdown strong. */
	t = T(90, LL_BATTERY_CHARGING, 0, LL_THERMAL_NOMINAL,
	      LL_PRESSURE_SERIOUS, CAM);
	CHECK(lenslink_telemetry_severity(&t) == LL_SEVERITY_WARNING);
	t.system_pressure = LL_PRESSURE_CRITICAL;
	CHECK(lenslink_telemetry_severity(&t) == LL_SEVERITY_CRITICAL);
	t.system_pressure = LL_PRESSURE_SHUTDOWN;
	CHECK(lenslink_telemetry_severity(&t) == LL_SEVERITY_CRITICAL);

	/* What is behind it — never "thermal" without a temperature. */
	t = T(90, LL_BATTERY_CHARGING, 0, LL_THERMAL_SERIOUS,
	      LL_PRESSURE_SERIOUS, CAM);
	CHECK(lenslink_telemetry_pressure_cause(&t) == LL_PRESSURE_CAUSE_THERMAL);
	t.system_pressure_factors = SYS;
	CHECK(lenslink_telemetry_pressure_cause(&t) == LL_PRESSURE_CAUSE_THERMAL);
	t.system_pressure_factors = DEPTH;
	CHECK(lenslink_telemetry_pressure_cause(&t) == LL_PRESSURE_CAUSE_THERMAL);
	/* Peak power only, on a cool device: power, not heat. */
	t = T(90, LL_BATTERY_UNPLUGGED, 0, LL_THERMAL_NOMINAL,
	      LL_PRESSURE_SERIOUS, PEAK);
	CHECK(lenslink_telemetry_pressure_cause(&t) == LL_PRESSURE_CAUSE_POWER);
	t.system_pressure_factors = BSTRESS;
	CHECK(lenslink_telemetry_pressure_cause(&t) == LL_PRESSURE_CAUSE_POWER);
	t.system_pressure_factors = PEAK | CAM;
	CHECK(lenslink_telemetry_pressure_cause(&t) ==
	      LL_PRESSURE_CAUSE_THERMAL_AND_POWER);
	/* No factor information: say so, don't claim heat — even when the
	 * whole device runs hot, which is a different fact. */
	t = T(90, LL_BATTERY_UNPLUGGED, 0, LL_THERMAL_SERIOUS,
	      LL_PRESSURE_SERIOUS, 0);
	CHECK(lenslink_telemetry_pressure_cause(&t) ==
	      LL_PRESSURE_CAUSE_UNSPECIFIED);
	/* A cause is only asked of an elevated pressure. */
	t = T(90, LL_BATTERY_UNPLUGGED, 0, LL_THERMAL_CRITICAL,
	      LL_PRESSURE_FAIR, CAM);
	CHECK(lenslink_telemetry_pressure_cause(&t) == LL_PRESSURE_CAUSE_NONE);
	t = T(90, LL_BATTERY_UNPLUGGED, 0, 0, LL_PRESSURE_UNKNOWN, 0);
	CHECK(lenslink_telemetry_pressure_cause(&t) == LL_PRESSURE_CAUSE_NONE);
}

static void test_telemetry_format(void)
{
	struct lenslink_telemetry t;
	char buf[192];
	t = T(87, LL_BATTERY_CHARGING, 0, LL_THERMAL_NOMINAL,
	      LL_PRESSURE_SERIOUS, LL_PRESSURE_FACTOR_CAMERA_TEMPERATURE);
	lenslink_telemetry_format(&t, buf, sizeof(buf));
	CHECK_STR(buf, "battery=87 charging lpm=0 thermal=nominal "
		       "pressure=serious factors=cameraTemperature");
	t = T(-1, LL_BATTERY_UNKNOWN, -1, 0, 0, 0);
	lenslink_telemetry_format(&t, buf, sizeof(buf));
	CHECK_STR(buf, "battery=? unknown lpm=? thermal=unknown "
		       "pressure=unknown");
	t = T(19, LL_BATTERY_UNPLUGGED, 1, LL_THERMAL_FAIR, LL_PRESSURE_CRITICAL,
	      LL_PRESSURE_FACTOR_SYSTEM_TEMPERATURE |
		      LL_PRESSURE_FACTOR_PEAK_POWER);
	lenslink_telemetry_format(&t, buf, sizeof(buf));
	CHECK_STR(buf, "battery=19 unplugged lpm=1 thermal=fair "
		       "pressure=critical "
		       "factors=systemTemperature,peakPower");
	/* Truncates safely into any buffer. */
	for (size_t size = 0; size < 100; size++) {
		char small[100];
		memset(small, 'z', sizeof(small));
		lenslink_telemetry_format(&t, small, size);
		if (size)
			CHECK(memchr(small, 0, size) != NULL);
		CHECK(small[size] == 'z' || size == sizeof(small));
	}
}

/* Every truncation of a Stage 3 snapshot is rejected and leaves no
 * telemetry behind — never a trusted fragment. */
static void test_telemetry_truncation(void)
{
	struct lenslink_camera_state st;
	size_t len = strlen(telemetry_snapshot);
	for (size_t n = 0; n < len; n++) {
		memset(&st, 0xAB, sizeof(st));
		CHECK(!lenslink_camera_state_parse(telemetry_snapshot, n, &st));
		const struct lenslink_telemetry none = {0};
		CHECK(!st.valid && memcmp(&st.telemetry, &none, sizeof(none)) == 0);
	}
	CHECK(lenslink_camera_state_parse(telemetry_snapshot, len, &st));

	/* And the same over the full Stage 2 snapshot with telemetry on
	 * the end. */
	static char json[4096];
	size_t n = strlen(full_snapshot);
	memcpy(json, full_snapshot, n - 1);
	int extra = snprintf(json + n - 1, sizeof(json) - n + 1,
			     ",\"batteryPercent\":87,\"systemPressure\":"
			     "\"shutdown\",\"systemPressureFactors\":"
			     "[\"peakPower\"]}");
	size_t total = n - 1 + (size_t)extra;
	for (size_t i = 0; i < total; i++)
		CHECK(!lenslink_camera_state_parse(json, i, &st) && !st.valid);
	CHECK(lenslink_camera_state_parse(json, total, &st));
}

int main(void)
{
	test_full_snapshot();
	test_old_app();
	test_empty_and_unknown();
	test_pause_and_color();
	test_wrong_types();
	test_clamping();
	test_capacity();
	test_malformed();
	test_telemetry_full();
	test_telemetry_old_app();
	test_telemetry_battery_percent();
	test_telemetry_battery_state();
	test_telemetry_low_power_mode();
	test_telemetry_thermal_and_pressure();
	test_telemetry_factors();
	test_telemetry_derivations();
	test_telemetry_format();
	test_telemetry_truncation();
	TEST_MAIN_END();
}
