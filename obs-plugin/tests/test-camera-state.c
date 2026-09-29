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
	TEST_MAIN_END();
}
