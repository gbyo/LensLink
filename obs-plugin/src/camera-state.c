#include "camera-state.h"
#include "json-reader.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* Generous physical bounds: they exist to keep a hostile or corrupted
 * snapshot from feeding NaN/inf/absurd values into sliders and labels,
 * not to second-guess what the phone reports. */
#define ZOOM_MAX 1000.0
#define EV_LIMIT 16.0
#define ISO_MAX 1000000.0
#define SHUTTER_MAX 60.0
#define KELVIN_MIN 1000.0
#define KELVIN_MAX 40000.0
#define FPS_MAX 1000
#define DISTANCE_MAX 100.0

static double clampd(double v, double lo, double hi)
{
	return v < lo ? lo : v > hi ? hi : v;
}

/* Reads a number into *field (clamped) and sets `bit` in st->has. A
 * value of the wrong type, or a non-finite one, is skipped: unknown. */
static bool read_num(struct json_reader *r, struct lenslink_camera_state *st,
		     double *field, double lo, double hi, uint32_t bit)
{
	double v;
	if (json_peek(r) != JSON_TYPE_NUMBER)
		return json_skip(r);
	if (!json_read_number(r, &v))
		return false;
	if (!isfinite(v))
		return true;
	*field = clampd(v, lo, hi);
	st->has |= bit;
	return true;
}

/* One half of a min/max pair: sets `bit` in *range_bits only when a
 * finite number was actually read. */
static bool read_range_half(struct json_reader *r, double *field, double lo,
			    double hi, uint32_t *range_bits, uint32_t bit)
{
	double v;
	if (json_peek(r) != JSON_TYPE_NUMBER)
		return json_skip(r);
	if (!json_read_number(r, &v))
		return false;
	if (isfinite(v)) {
		*field = clampd(v, lo, hi);
		*range_bits |= bit;
	}
	return true;
}

static bool read_bool(struct json_reader *r, bool *field)
{
	if (json_peek(r) != JSON_TYPE_BOOL)
		return json_skip(r);
	return json_read_bool(r, field);
}

static bool read_str(struct json_reader *r, char *field, size_t size)
{
	if (json_peek(r) != JSON_TYPE_STRING)
		return json_skip(r);
	return json_read_string(r, field, size, NULL);
}

/* Reads a string enum value; unknown strings map to `fallback`. */
struct enum_name {
	const char *name;
	int value;
};

static bool read_enum(struct json_reader *r, int *field,
		      const struct enum_name *names, size_t count, int fallback)
{
	char buf[32];
	bool truncated = false;
	if (json_peek(r) != JSON_TYPE_STRING)
		return json_skip(r);
	if (!json_read_string(r, buf, sizeof(buf), &truncated))
		return false;
	*field = fallback;
	if (truncated)
		return true;
	for (size_t i = 0; i < count; i++) {
		if (strcmp(buf, names[i].name) == 0) {
			*field = names[i].value;
			break;
		}
	}
	return true;
}

/* An array of strings into a fixed table; extra items and non-string
 * items are skipped. */
static bool read_str_list(struct json_reader *r, char *table, size_t stride,
			  size_t max, size_t *count)
{
	*count = 0;
	if (json_peek(r) != JSON_TYPE_ARRAY)
		return json_skip(r);
	if (!json_array_begin(r))
		return false;
	while (json_array_next(r)) {
		if (*count < max && json_peek(r) == JSON_TYPE_STRING) {
			if (!json_read_string(r, table + *count * stride,
					      stride, NULL))
				return false;
			(*count)++;
		} else if (!json_skip(r)) {
			return false;
		}
	}
	return !r->error;
}

static bool read_int_list(struct json_reader *r, int *table, size_t max,
			  size_t *count, int lo, int hi)
{
	*count = 0;
	if (json_peek(r) != JSON_TYPE_ARRAY)
		return json_skip(r);
	if (!json_array_begin(r))
		return false;
	while (json_array_next(r)) {
		double v;
		if (*count < max && json_peek(r) == JSON_TYPE_NUMBER) {
			if (!json_read_number(r, &v))
				return false;
			if (isfinite(v))
				table[(*count)++] =
					(int)clampd(v, (double)lo, (double)hi);
		} else if (!json_skip(r)) {
			return false;
		}
	}
	return !r->error;
}

/* "mics": [{"id": "...", "name": "..."}, ...]. Entries without an id
 * can't be selected, so they're dropped. */
static bool read_mics(struct json_reader *r, struct lenslink_camera_state *st)
{
	st->mic_count = 0;
	if (json_peek(r) != JSON_TYPE_ARRAY)
		return json_skip(r);
	if (!json_array_begin(r))
		return false;
	while (json_array_next(r)) {
		if (st->mic_count >= LLCS_MAX_MICS ||
		    json_peek(r) != JSON_TYPE_OBJECT) {
			if (!json_skip(r))
				return false;
			continue;
		}
		struct lenslink_mic_option m;
		memset(&m, 0, sizeof(m));
		bool id_truncated = false;
		char key[16];
		bool key_truncated;
		if (!json_object_begin(r))
			return false;
		while (json_object_next(r, key, sizeof(key), &key_truncated)) {
			bool ok;
			if (!key_truncated && strcmp(key, "id") == 0 &&
			    json_peek(r) == JSON_TYPE_STRING)
				ok = json_read_string(r, m.id, sizeof(m.id),
						      &id_truncated);
			else if (!key_truncated && strcmp(key, "name") == 0)
				ok = read_str(r, m.name, sizeof(m.name));
			else
				ok = json_skip(r);
			if (!ok)
				return false;
		}
		if (r->error)
			return false;
		/* A truncated id would select nothing (or the wrong mic). */
		if (m.id[0] && !id_truncated)
			st->mics[st->mic_count++] = m;
	}
	return !r->error;
}

static const struct enum_name focus_names[] = {
	{"auto", LL_FOCUS_AUTO},
	{"locked", LL_FOCUS_LOCKED},
};
static const struct enum_name exposure_names[] = {
	{"auto", LL_EXPOSURE_AUTO},
	{"manual", LL_EXPOSURE_MANUAL},
};
static const struct enum_name wb_names[] = {
	{"auto", LL_WB_AUTO},
	{"locked", LL_WB_LOCKED},
};
static const struct enum_name camera_names[] = {
	{"back", LL_CAMERA_BACK},
	{"front", LL_CAMERA_FRONT},
};
static const struct enum_name pause_names[] = {
	{"", LL_PAUSE_NONE},
	{"user", LL_PAUSE_USER},
	{"camera", LL_PAUSE_CAMERA},
};

#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

/* --- Telemetry ------------------------------------------------------------
 *
 * Unlike the camera-control fields above, a bad telemetry value must leave
 * its field *unknown* — even when an earlier duplicate key already set it —
 * so each reader resets its field first (the last key wins, whatever its
 * type) and sets it only from a value that is valid as sent. Nothing is
 * clamped: an impossible battery percentage is not a possible one. */

static const struct enum_name battery_state_names[] = {
	{"unplugged", LL_BATTERY_UNPLUGGED},
	{"charging", LL_BATTERY_CHARGING},
	{"full", LL_BATTERY_FULL},
};
static const struct enum_name thermal_state_names[] = {
	{"nominal", LL_THERMAL_NOMINAL},
	{"fair", LL_THERMAL_FAIR},
	{"serious", LL_THERMAL_SERIOUS},
	{"critical", LL_THERMAL_CRITICAL},
};
static const struct enum_name system_pressure_names[] = {
	{"nominal", LL_PRESSURE_NOMINAL},
	{"fair", LL_PRESSURE_FAIR},
	{"serious", LL_PRESSURE_SERIOUS},
	{"critical", LL_PRESSURE_CRITICAL},
	{"shutdown", LL_PRESSURE_SHUTDOWN},
};
/* In the order the app sends (and diagnostics print) them. */
static const struct enum_name pressure_factor_names[] = {
	{"systemTemperature", LL_PRESSURE_FACTOR_SYSTEM_TEMPERATURE},
	{"cameraTemperature", LL_PRESSURE_FACTOR_CAMERA_TEMPERATURE},
	{"depthModuleTemperature", LL_PRESSURE_FACTOR_DEPTH_MODULE_TEMPERATURE},
	{"peakPower", LL_PRESSURE_FACTOR_PEAK_POWER},
	{"batteryStress", LL_PRESSURE_FACTOR_BATTERY_STRESS},
};

#define LL_THERMAL_FACTORS                                      \
	(LL_PRESSURE_FACTOR_SYSTEM_TEMPERATURE |                \
	 LL_PRESSURE_FACTOR_CAMERA_TEMPERATURE |                \
	 LL_PRESSURE_FACTOR_DEPTH_MODULE_TEMPERATURE)
#define LL_POWER_FACTORS \
	(LL_PRESSURE_FACTOR_PEAK_POWER | LL_PRESSURE_FACTOR_BATTERY_STRESS)

/* An integer 0..100, or unknown. */
static bool read_battery_percent(struct json_reader *r,
				 struct lenslink_telemetry *t)
{
	double v;
	t->has_battery_percent = false;
	t->battery_percent = 0;
	if (json_peek(r) != JSON_TYPE_NUMBER)
		return json_skip(r);
	if (!json_read_number(r, &v))
		return false;
	if (isfinite(v) && v >= 0.0 && v <= 100.0 && v == floor(v)) {
		t->battery_percent = (int)v;
		t->has_battery_percent = true;
	}
	return true;
}

static bool read_low_power_mode(struct json_reader *r,
				struct lenslink_telemetry *t)
{
	t->has_low_power_mode = false;
	t->low_power_mode = false;
	if (json_peek(r) != JSON_TYPE_BOOL)
		return json_skip(r);
	if (!json_read_bool(r, &t->low_power_mode))
		return false;
	t->has_low_power_mode = true;
	return true;
}

/* A string enum whose every failure — wrong type, unknown or oversized
 * name — is 0, the UNKNOWN value each telemetry enum starts with. */
static bool read_telemetry_enum(struct json_reader *r, int *field,
				const struct enum_name *names, size_t count)
{
	int e = 0;
	bool ok = read_enum(r, &e, names, count, 0);
	*field = e;
	return ok;
}

/* "systemPressureFactors": ["cameraTemperature", ...] into a bitmask.
 * Unknown names, non-strings and repeats are ignored (a factor a future
 * app adds must not cost the ones we know); the array's length is
 * bounded by the payload itself. */
static bool read_pressure_factors(struct json_reader *r, uint32_t *bits)
{
	*bits = 0;
	if (json_peek(r) != JSON_TYPE_ARRAY)
		return json_skip(r);
	if (!json_array_begin(r))
		return false;
	while (json_array_next(r)) {
		char name[32];
		bool truncated = false;
		if (json_peek(r) != JSON_TYPE_STRING) {
			if (!json_skip(r))
				return false;
			continue;
		}
		if (!json_read_string(r, name, sizeof(name), &truncated))
			return false;
		if (truncated)
			continue;
		for (size_t i = 0; i < COUNT(pressure_factor_names); i++) {
			if (strcmp(name, pressure_factor_names[i].name) == 0) {
				*bits |= (uint32_t)pressure_factor_names[i].value;
				break;
			}
		}
	}
	return !r->error;
}

/* Returns true when `key` was a telemetry key (its value consumed, into
 * *ok). */
static bool read_telemetry_field(struct json_reader *r, const char *key,
				 struct lenslink_telemetry *t, bool *ok)
{
	int e;

#define IS(k) (strcmp(key, k) == 0)
	if (IS("batteryPercent")) {
		*ok = read_battery_percent(r, t);
	} else if (IS("batteryState")) {
		*ok = read_telemetry_enum(r, &e, battery_state_names,
					  COUNT(battery_state_names));
		t->battery_state = (enum lenslink_battery_state)e;
	} else if (IS("lowPowerMode")) {
		*ok = read_low_power_mode(r, t);
	} else if (IS("thermalState")) {
		*ok = read_telemetry_enum(r, &e, thermal_state_names,
					  COUNT(thermal_state_names));
		t->thermal_state = (enum lenslink_thermal_state)e;
	} else if (IS("systemPressure")) {
		*ok = read_telemetry_enum(r, &e, system_pressure_names,
					  COUNT(system_pressure_names));
		t->system_pressure = (enum lenslink_system_pressure)e;
	} else if (IS("systemPressureFactors")) {
		*ok = read_pressure_factors(r, &t->system_pressure_factors);
	} else {
		return false;
	}
#undef IS
	return true;
}

static bool read_field(struct json_reader *r, const char *key,
		       struct lenslink_camera_state *st, double *min_iso,
		       double *max_iso, double *min_shutter,
		       double *max_shutter, uint32_t *range_bits)
{
	int e;
	bool telemetry_ok;

	if (read_telemetry_field(r, key, &st->telemetry, &telemetry_ok))
		return telemetry_ok;

#define IS(k) (strcmp(key, k) == 0)
	if (IS("paused"))
		return read_bool(r, &st->paused);
	if (IS("pauseReason")) {
		e = (int)st->pause_reason;
		bool ok = read_enum(r, &e, pause_names, COUNT(pause_names),
				    LL_PAUSE_OTHER);
		st->pause_reason = (enum lenslink_pause_reason)e;
		return ok;
	}
	if (IS("zoom"))
		return read_num(r, st, &st->zoom, 0.0, ZOOM_MAX,
				LLCS_HAS_ZOOM);
	if (IS("maxZoom"))
		return read_num(r, st, &st->max_zoom, 0.0, ZOOM_MAX,
				LLCS_HAS_MAX_ZOOM);
	if (IS("exposureBias"))
		return read_num(r, st, &st->exposure_bias, -EV_LIMIT,
				EV_LIMIT, LLCS_HAS_EXPOSURE_BIAS);
	if (IS("focusMode")) {
		e = (int)st->focus_mode;
		bool ok = read_enum(r, &e, focus_names, COUNT(focus_names),
				    LL_FOCUS_UNKNOWN);
		st->focus_mode = (enum lenslink_focus_mode)e;
		return ok;
	}
	if (IS("lensPosition"))
		return read_num(r, st, &st->lens_position, 0.0, 1.0,
				LLCS_HAS_LENS_POSITION);
	if (IS("faceFocus"))
		return read_bool(r, &st->face_focus);
	if (IS("supportsFaceFocus"))
		return read_bool(r, &st->supports_face_focus);
	if (IS("flashlight"))
		return read_bool(r, &st->flashlight);
	if (IS("hasFlashlight"))
		return read_bool(r, &st->has_flashlight);
	if (IS("camera")) {
		e = (int)st->camera;
		bool ok = read_enum(r, &e, camera_names, COUNT(camera_names),
				    LL_CAMERA_UNKNOWN);
		st->camera = (enum lenslink_camera_position)e;
		return ok;
	}
	if (IS("lens"))
		return read_str(r, st->lens, sizeof(st->lens));
	if (IS("lenses"))
		return read_str_list(r, &st->lenses[0][0],
				     sizeof(st->lenses[0]), LLCS_MAX_LENSES,
				     &st->lens_count);
	if (IS("whiteBalanceMode")) {
		e = (int)st->wb_mode;
		bool ok = read_enum(r, &e, wb_names, COUNT(wb_names),
				    LL_WB_UNKNOWN);
		st->wb_mode = (enum lenslink_wb_mode)e;
		return ok;
	}
	if (IS("whiteBalanceTemperature"))
		return read_num(r, st, &st->wb_temperature, KELVIN_MIN,
				KELVIN_MAX, LLCS_HAS_WB_TEMPERATURE);
	if (IS("supportsWhiteBalanceLock"))
		return read_bool(r, &st->supports_wb_lock);
	if (IS("exposureMode")) {
		e = (int)st->exposure_mode;
		bool ok = read_enum(r, &e, exposure_names,
				    COUNT(exposure_names), LL_EXPOSURE_UNKNOWN);
		st->exposure_mode = (enum lenslink_exposure_mode)e;
		return ok;
	}
	if (IS("iso"))
		return read_num(r, st, &st->iso, 0.0, ISO_MAX, LLCS_HAS_ISO);
	/* Range halves land in temporaries; the pair is committed only
	 * when both arrived (see lenslink_camera_state_parse). */
	if (IS("minISO"))
		return read_range_half(r, min_iso, 0.0, ISO_MAX, range_bits,
				       1u);
	if (IS("maxISO"))
		return read_range_half(r, max_iso, 0.0, ISO_MAX, range_bits,
				       2u);
	if (IS("shutterSeconds"))
		return read_num(r, st, &st->shutter_seconds, 0.0, SHUTTER_MAX,
				LLCS_HAS_SHUTTER);
	if (IS("minShutterSeconds"))
		return read_range_half(r, min_shutter, 0.0, SHUTTER_MAX, range_bits,
				       4u);
	if (IS("maxShutterSeconds"))
		return read_range_half(r, max_shutter, 0.0, SHUTTER_MAX, range_bits,
				       8u);
	if (IS("supportsManualExposure"))
		return read_bool(r, &st->supports_manual_exposure);
	if (IS("resolution"))
		return read_str(r, st->resolution, sizeof(st->resolution));
	if (IS("fps")) {
		double fps = 0.0;
		uint32_t before = st->has;
		if (!read_num(r, st, &fps, 0.0, FPS_MAX, LLCS_HAS_FPS))
			return false;
		if (st->has != before)
			st->fps = (int)(fps + 0.5);
		return true;
	}
	if (IS("codec"))
		return read_str(r, st->codec, sizeof(st->codec));
	if (IS("quality"))
		return read_str(r, st->quality, sizeof(st->quality));
	if (IS("resolutions"))
		return read_str_list(r, &st->resolutions[0][0],
				     sizeof(st->resolutions[0]),
				     LLCS_MAX_RESOLUTIONS,
				     &st->resolution_count);
	if (IS("frameRates"))
		return read_int_list(r, st->frame_rates, LLCS_MAX_FRAME_RATES,
				     &st->frame_rate_count, 0, FPS_MAX);
	if (IS("codecs"))
		return read_str_list(r, &st->codecs[0][0],
				     sizeof(st->codecs[0]), LLCS_MAX_CODECS,
				     &st->codec_count);
	if (IS("hdr"))
		return read_bool(r, &st->hdr);
	if (IS("color")) {
		/* Presence is what matters (any colour key means a 10-bit
		 * pipeline); the value is kept when it's a string. */
		st->has |= LLCS_HAS_COLOR;
		return read_str(r, st->color, sizeof(st->color));
	}
	if (IS("supportsGreenScreen"))
		return read_bool(r, &st->supports_green_screen);
	if (IS("greenScreen"))
		return read_bool(r, &st->green_screen);
	if (IS("greenScreenDepth"))
		return read_bool(r, &st->green_screen_depth);
	if (IS("greenScreenMaxDistance"))
		return read_num(r, st, &st->green_screen_max_distance, 0.0,
				DISTANCE_MAX, LLCS_HAS_GS_MAX_DISTANCE);
	if (IS("micEnabled"))
		return read_bool(r, &st->mic_enabled);
	if (IS("mic"))
		return read_str(r, st->mic, sizeof(st->mic));
	if (IS("mics"))
		return read_mics(r, st);
#undef IS

	return json_skip(r);
}

bool lenslink_camera_state_parse(const char *json, size_t len,
				 struct lenslink_camera_state *out)
{
	struct lenslink_camera_state st;
	struct json_reader r;
	double min_iso = 0.0, max_iso = 0.0;
	double min_shutter = 0.0, max_shutter = 0.0;
	uint32_t range_bits = 0;
	char key[48];
	bool key_truncated;

	memset(&st, 0, sizeof(st));
	memset(out, 0, sizeof(*out));

	json_reader_init(&r, json, len);
	if (!json_object_begin(&r))
		return false;
	while (json_object_next(&r, key, sizeof(key), &key_truncated)) {
		bool ok = key_truncated
				  ? json_skip(&r)
				  : read_field(&r, key, &st, &min_iso,
					       &max_iso, &min_shutter,
					       &max_shutter, &range_bits);
		if (!ok)
			return false;
	}
	if (!json_reader_done(&r))
		return false;

	if ((range_bits & 3u) == 3u) {
		st.min_iso = min_iso <= max_iso ? min_iso : max_iso;
		st.max_iso = min_iso <= max_iso ? max_iso : min_iso;
		st.has |= LLCS_HAS_ISO_RANGE;
	}
	if ((range_bits & 12u) == 12u) {
		st.min_shutter_seconds =
			min_shutter <= max_shutter ? min_shutter : max_shutter;
		st.max_shutter_seconds =
			min_shutter <= max_shutter ? max_shutter : min_shutter;
		st.has |= LLCS_HAS_SHUTTER_RANGE;
	}
	/* "paused": false with a leftover reason means not paused. */
	if (!st.paused)
		st.pause_reason = LL_PAUSE_NONE;
	/* Factors explain a pressure level; with no level to explain they are
	 * a fragment of something malformed, not a fact. */
	if (st.telemetry.system_pressure == LL_PRESSURE_UNKNOWN)
		st.telemetry.system_pressure_factors = 0;

	st.valid = true;
	*out = st;
	return true;
}

/* --- Derived classifications and names ------------------------------------ */

static bool pressure_elevated(enum lenslink_system_pressure p)
{
	return p == LL_PRESSURE_SERIOUS || p == LL_PRESSURE_CRITICAL ||
	       p == LL_PRESSURE_SHUTDOWN;
}

bool lenslink_telemetry_any(const struct lenslink_telemetry *t)
{
	return t->has_battery_percent ||
	       t->battery_state != LL_BATTERY_UNKNOWN ||
	       t->has_low_power_mode || t->thermal_state != LL_THERMAL_UNKNOWN ||
	       t->system_pressure != LL_PRESSURE_UNKNOWN;
}

enum lenslink_pressure_cause
lenslink_telemetry_pressure_cause(const struct lenslink_telemetry *t)
{
	if (!pressure_elevated(t->system_pressure))
		return LL_PRESSURE_CAUSE_NONE;
	bool thermal = (t->system_pressure_factors & LL_THERMAL_FACTORS) != 0;
	bool power = (t->system_pressure_factors & LL_POWER_FACTORS) != 0;
	if (thermal && power)
		return LL_PRESSURE_CAUSE_THERMAL_AND_POWER;
	if (thermal)
		return LL_PRESSURE_CAUSE_THERMAL;
	if (power)
		return LL_PRESSURE_CAUSE_POWER;
	return LL_PRESSURE_CAUSE_UNSPECIFIED;
}

bool lenslink_telemetry_battery_low(const struct lenslink_telemetry *t)
{
	return t->has_battery_percent &&
	       t->battery_state == LL_BATTERY_UNPLUGGED &&
	       t->battery_percent <= LL_BATTERY_LOW_PERCENT;
}

enum lenslink_telemetry_severity
lenslink_telemetry_severity(const struct lenslink_telemetry *t)
{
	if (t->thermal_state == LL_THERMAL_CRITICAL ||
	    t->system_pressure == LL_PRESSURE_CRITICAL ||
	    t->system_pressure == LL_PRESSURE_SHUTDOWN)
		return LL_SEVERITY_CRITICAL;
	if (lenslink_telemetry_battery_low(t) ||
	    t->thermal_state == LL_THERMAL_SERIOUS ||
	    t->system_pressure == LL_PRESSURE_SERIOUS)
		return LL_SEVERITY_WARNING;
	if (t->has_low_power_mode && t->low_power_mode)
		return LL_SEVERITY_INFO;
	return LL_SEVERITY_NORMAL;
}

static const char *name_of(const struct enum_name *names, size_t count,
			   int value)
{
	for (size_t i = 0; i < count; i++)
		if (names[i].value == value)
			return names[i].name;
	return "unknown";
}

const char *lenslink_battery_state_name(enum lenslink_battery_state s)
{
	return name_of(battery_state_names, COUNT(battery_state_names), s);
}

const char *lenslink_thermal_state_name(enum lenslink_thermal_state s)
{
	return name_of(thermal_state_names, COUNT(thermal_state_names), s);
}

const char *lenslink_system_pressure_name(enum lenslink_system_pressure s)
{
	return name_of(system_pressure_names, COUNT(system_pressure_names), s);
}

const char *lenslink_pressure_cause_name(enum lenslink_pressure_cause c)
{
	switch (c) {
	case LL_PRESSURE_CAUSE_UNSPECIFIED:
		return "unspecified";
	case LL_PRESSURE_CAUSE_THERMAL:
		return "thermal";
	case LL_PRESSURE_CAUSE_POWER:
		return "power";
	case LL_PRESSURE_CAUSE_THERMAL_AND_POWER:
		return "thermal+power";
	case LL_PRESSURE_CAUSE_NONE:
		break;
	}
	return "none";
}

void lenslink_telemetry_format(const struct lenslink_telemetry *t, char *buf,
			       size_t size)
{
	char percent[8] = "?";
	char lpm[2] = "?";
	if (!size)
		return;
	if (t->has_battery_percent)
		snprintf(percent, sizeof(percent), "%d", t->battery_percent);
	if (t->has_low_power_mode)
		lpm[0] = t->low_power_mode ? '1' : '0';
	int n = snprintf(buf, size,
			 "battery=%s %s lpm=%s thermal=%s pressure=%s", percent,
			 lenslink_battery_state_name(t->battery_state), lpm,
			 lenslink_thermal_state_name(t->thermal_state),
			 lenslink_system_pressure_name(t->system_pressure));
	if (n < 0 || (size_t)n >= size)
		return;
	const char *sep = " factors=";
	for (size_t i = 0; i < COUNT(pressure_factor_names); i++) {
		if (!(t->system_pressure_factors &
		      (uint32_t)pressure_factor_names[i].value))
			continue;
		int w = snprintf(buf + n, size - (size_t)n, "%s%s", sep,
				 pressure_factor_names[i].name);
		if (w < 0 || (size_t)w >= size - (size_t)n)
			return;
		n += w;
		sep = ",";
	}
}
