#include "camera-state.h"
#include "json-reader.h"

#include <math.h>
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

static bool read_field(struct json_reader *r, const char *key,
		       struct lenslink_camera_state *st, double *min_iso,
		       double *max_iso, double *min_shutter,
		       double *max_shutter, uint32_t *range_bits)
{
	int e;

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

	st.valid = true;
	*out = st;
	return true;
}
