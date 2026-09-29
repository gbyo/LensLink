/*
 * The phone's JSON packets — HELLO, VIDEO_CONFIG, STATE — through their
 * real parsers, plus a raw reader walk. Invariants: every string that
 * comes out is terminated, valid UTF-8, and NUL-free; a device id is
 * either absent or canonical; an invalid packet leaves nothing behind.
 */

#include "fuzz.h"

#include "camera-state.h"
#include "handshake.h"
#include "json-reader.h"

#include <string.h>

static const struct fuzz_seed seeds[] = {
	FUZZ_SEED("{\"name\":\"Gibson\\u2019s iPhone\",\"deviceID\":"
		  "\"B1092868-1234-5678-ABCD-0123456789AB\",\"app\":"
		  "\"LensLink\",\"protocol\":1,\"kind\":\"camera\","
		  "\"standby\":true}"),
	FUZZ_SEED("{\"name\":\"Emma's iPhone\",\"app\":\"LensLink\","
		  "\"protocol\":1,\"kind\":\"screen\"}"),
	FUZZ_SEED("{\"codec\":\"hevc\",\"width\":1920,\"height\":1080,"
		  "\"fps\":30,\"kind\":\"camera\",\"color\":\"hlg\"}"),
	FUZZ_SEED("{\"paused\":false,\"zoom\":2.5,\"lenses\":[\"Ultra Wide "
		  "(0.5\xC3\x97)\",\"Main\"],\"frameRates\":[24,30,60],"
		  "\"mics\":[{\"id\":\"auto\",\"name\":\"A\\u00d7\\ud83d"
		  "\\ude00\"}],\"iso\":1e3,\"future\":{\"a\":[1,{\"b\":null}]}}"),
	FUZZ_SEED("[[[[{\"a\":\"\\\"\\u0000\"}]]]]"),
	FUZZ_SEED("{\"minISO\":34,\"maxISO\":3072,\"color\":\"log\"}"),
	/* Phone telemetry: a normal one, a pressured one with every factor
	 * (and a future one), and impossible/mistyped values. */
	FUZZ_SEED("{\"batteryPercent\":87,\"batteryState\":\"charging\","
		  "\"lowPowerMode\":false,\"thermalState\":\"nominal\","
		  "\"systemPressure\":\"nominal\"}"),
	FUZZ_SEED("{\"batteryPercent\":19,\"batteryState\":\"unplugged\","
		  "\"lowPowerMode\":true,\"thermalState\":\"critical\","
		  "\"systemPressure\":\"shutdown\",\"systemPressureFactors\":"
		  "[\"systemTemperature\",\"cameraTemperature\","
		  "\"depthModuleTemperature\",\"peakPower\","
		  "\"batteryStress\",\"future\"]}"),
	FUZZ_SEED("{\"batteryPercent\":140.5,\"batteryPercent\":\"9\","
		  "\"batteryState\":7,\"lowPowerMode\":null,"
		  "\"thermalState\":\"lava\",\"systemPressure\":\"serious\","
		  "\"systemPressureFactors\":[1,null,[\"peakPower\"],"
		  "\"peakPower\",\"\\u0000\"]}"),
};

const struct fuzz_seed *fuzz_seeds(size_t *count)
{
	*count = sizeof(seeds) / sizeof(seeds[0]);
	return seeds;
}

static void check_hello(const uint8_t *data, size_t size)
{
	struct lenslink_hello h;
	bool ok = lenslink_hello_parse((const char *)data, size, &h);
	FUZZ_ASSERT(ok == h.valid);
	FUZZ_ASSERT(fuzz_utf8_field_ok(h.name, sizeof(h.name)));
	FUZZ_ASSERT(fuzz_utf8_field_ok(h.app, sizeof(h.app)));
	FUZZ_ASSERT(h.device_id[0] == 0 ||
		    (strlen(h.device_id) == LENSLINK_DEVICE_ID_LEN &&
		     lenslink_device_id_valid(h.device_id)));
	/* Canonical means upper case: re-parsing is the identity. */
	if (h.device_id[0]) {
		char again[LENSLINK_DEVICE_ID_SIZE];
		FUZZ_ASSERT(lenslink_device_id_parse(h.device_id,
						     LENSLINK_DEVICE_ID_LEN,
						     again) &&
			    strcmp(again, h.device_id) == 0);
	}
	FUZZ_ASSERT(h.protocol >= 0 && h.protocol <= 255);
	if (!ok)
		FUZZ_ASSERT(!h.name[0] && !h.app[0] && !h.protocol &&
			    !h.is_screen && !h.standby && !h.device_id[0]);
}

static void check_video_config(const uint8_t *data, size_t size)
{
	struct lenslink_video_config c;
	bool ok = lenslink_video_config_parse((const char *)data, size, &c);
	FUZZ_ASSERT(ok == c.valid);
	FUZZ_ASSERT(c.codec == LENSLINK_CODEC_H264 ||
		    c.codec == LENSLINK_CODEC_HEVC);
	if (!ok)
		FUZZ_ASSERT(!c.has_kind && c.codec == LENSLINK_CODEC_H264);
}

static void check_state(const uint8_t *data, size_t size)
{
	struct lenslink_camera_state st;
	bool ok = lenslink_camera_state_parse((const char *)data, size, &st);
	FUZZ_ASSERT(ok == st.valid);
	if (!ok) {
		const struct lenslink_telemetry none = {0};
		FUZZ_ASSERT(st.has == 0 && st.lens_count == 0);
		FUZZ_ASSERT(memcmp(&st.telemetry, &none, sizeof(none)) == 0);
		return;
	}
	FUZZ_ASSERT(st.lens_count <= LLCS_MAX_LENSES);
	for (size_t i = 0; i < st.lens_count; i++)
		FUZZ_ASSERT(fuzz_utf8_field_ok(st.lenses[i],
					       sizeof(st.lenses[i])));
	FUZZ_ASSERT(st.mic_count <= LLCS_MAX_MICS);
	for (size_t i = 0; i < st.mic_count; i++) {
		FUZZ_ASSERT(fuzz_utf8_field_ok(st.mics[i].id,
					       sizeof(st.mics[i].id)));
		FUZZ_ASSERT(fuzz_utf8_field_ok(st.mics[i].name,
					       sizeof(st.mics[i].name)));
	}
	FUZZ_ASSERT(fuzz_utf8_field_ok(st.lens, sizeof(st.lens)));

	/* Telemetry is always a valid fact or plainly unknown. */
	const struct lenslink_telemetry *t = &st.telemetry;
	FUZZ_ASSERT(t->has_battery_percent
			    ? t->battery_percent >= 0 && t->battery_percent <= 100
			    : t->battery_percent == 0);
	FUZZ_ASSERT(t->has_low_power_mode || !t->low_power_mode);
	FUZZ_ASSERT((int)t->battery_state >= LL_BATTERY_UNKNOWN &&
		    (int)t->battery_state <= LL_BATTERY_FULL);
	FUZZ_ASSERT((int)t->thermal_state >= LL_THERMAL_UNKNOWN &&
		    (int)t->thermal_state <= LL_THERMAL_CRITICAL);
	FUZZ_ASSERT((int)t->system_pressure >= LL_PRESSURE_UNKNOWN &&
		    (int)t->system_pressure <= LL_PRESSURE_SHUTDOWN);
	FUZZ_ASSERT((t->system_pressure_factors &
		     ~(LL_PRESSURE_FACTOR_BATTERY_STRESS * 2u - 1u)) == 0);
	FUZZ_ASSERT(t->system_pressure != LL_PRESSURE_UNKNOWN ||
		    t->system_pressure_factors == 0);
	/* The derived views never fail on whatever came out. */
	char line[192];
	lenslink_telemetry_format(t, line, sizeof(line));
	FUZZ_ASSERT(memchr(line, 0, sizeof(line)) != NULL);
	(void)lenslink_telemetry_severity(t);
	(void)lenslink_telemetry_pressure_cause(t);
}

/* A raw walk that reads every string into a small buffer. */
static void walk(struct json_reader *r, int budget)
{
	char buf[8];
	bool trunc;
	if (budget <= 0) {
		json_skip(r);
		return;
	}
	switch (json_peek(r)) {
	case JSON_TYPE_OBJECT:
		if (!json_object_begin(r))
			return;
		while (json_object_next(r, buf, sizeof(buf), &trunc)) {
			FUZZ_ASSERT(fuzz_utf8_field_ok(buf, sizeof(buf)));
			walk(r, budget - 1);
		}
		break;
	case JSON_TYPE_ARRAY:
		if (!json_array_begin(r))
			return;
		while (json_array_next(r))
			walk(r, budget - 1);
		break;
	case JSON_TYPE_STRING:
		if (json_read_string(r, buf, sizeof(buf), &trunc))
			FUZZ_ASSERT(fuzz_utf8_field_ok(buf, sizeof(buf)));
		break;
	default:
		json_skip(r);
		break;
	}
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	check_hello(data, size);
	check_video_config(data, size);
	check_state(data, size);

	struct json_reader r;
	json_reader_init(&r, (const char *)data, size);
	walk(&r, 64);
	(void)json_reader_done(&r);
	return 0;
}
