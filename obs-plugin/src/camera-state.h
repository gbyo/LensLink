/*
 * Typed camera STATE (docs/PROTOCOL.md §8).
 *
 * The app's STATE snapshot is JSON, and the raw text stays the protocol
 * and /api/state contract. But every native consumer — the source's own
 * status logic, the Qt dock, diagnostics — needs the same *interpretation*
 * of it, so the plugin parses each snapshot once, when it arrives, into
 * this struct. Nothing else parses STATE.
 *
 * Every field is optional on the wire: an older app omits newer keys, and
 * boolean keys are emitted only while true. So:
 *   - booleans read false when absent (absent = off / unsupported);
 *   - scalar values carry a LLCS_HAS_* bit — no bit means unknown, and the
 *     value is 0;
 *   - lists are empty when absent.
 * Values are clamped to sane ranges on parse; a malformed snapshot leaves
 * the whole struct unknown (valid == false) rather than half-filled.
 *
 * The phone-health telemetry (struct lenslink_telemetry) is the exception
 * to "clamped": a battery percentage of 140 is not "100%", it is a lie,
 * so an out-of-range or otherwise malformed telemetry value becomes
 * *unknown* — and only that field does, never the whole snapshot.
 *
 * Pure C, no libobs: unit-tested standalone (obs-plugin/tests).
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LLCS_MAX_LENSES 8
#define LLCS_MAX_RESOLUTIONS 8
#define LLCS_MAX_FRAME_RATES 8
#define LLCS_MAX_CODECS 4
#define LLCS_MAX_MICS 8

/* Scalar-presence bits for lenslink_camera_state.has. */
enum {
	LLCS_HAS_ZOOM = 1u << 0,
	LLCS_HAS_MAX_ZOOM = 1u << 1,
	LLCS_HAS_EXPOSURE_BIAS = 1u << 2,
	LLCS_HAS_LENS_POSITION = 1u << 3,
	LLCS_HAS_WB_TEMPERATURE = 1u << 4,
	LLCS_HAS_ISO = 1u << 5,
	LLCS_HAS_ISO_RANGE = 1u << 6,     /* both minISO and maxISO */
	LLCS_HAS_SHUTTER = 1u << 7,
	LLCS_HAS_SHUTTER_RANGE = 1u << 8, /* both min/maxShutterSeconds */
	LLCS_HAS_FPS = 1u << 9,
	LLCS_HAS_GS_MAX_DISTANCE = 1u << 10,
	LLCS_HAS_COLOR = 1u << 11, /* a "color" key (Apple Log today) */
};

enum lenslink_focus_mode {
	LL_FOCUS_UNKNOWN,
	LL_FOCUS_AUTO,
	LL_FOCUS_LOCKED,
};

enum lenslink_exposure_mode {
	LL_EXPOSURE_UNKNOWN,
	LL_EXPOSURE_AUTO,
	LL_EXPOSURE_MANUAL,
};

enum lenslink_wb_mode {
	LL_WB_UNKNOWN,
	LL_WB_AUTO,
	LL_WB_LOCKED,
};

enum lenslink_camera_position {
	LL_CAMERA_UNKNOWN,
	LL_CAMERA_BACK,
	LL_CAMERA_FRONT,
};

enum lenslink_pause_reason {
	LL_PAUSE_NONE,  /* not paused, or no reason given */
	LL_PAUSE_USER,  /* an operator paused it */
	LL_PAUSE_CAMERA, /* iOS interrupted capture */
	LL_PAUSE_OTHER, /* a reason this plugin doesn't know yet */
};

/* --- Telemetry (docs/PROTOCOL.md §8, "Telemetry") -------------------------
 *
 * The phone's power and capture health. Three facts that are never merged:
 *   - power (battery percent / plug state / Low Power Mode);
 *   - thermal_state: the whole *device's* thermal condition;
 *   - system_pressure: the *camera's* pressure — what LensLink's own
 *     throttling reacts to — with `system_pressure_factors` saying why.
 *     Serious pressure caused by peak power is not a thermal problem, so
 *     never call this one "thermal" without a temperature factor.
 *
 * Only facts live here. Wording and colour belong to each surface; the
 * derived helpers below classify severity without choosing either.
 * Everything is unknown (UNKNOWN / has_* false / 0) for an app that
 * doesn't send it — never a fabricated default. */

enum lenslink_battery_state {
	LL_BATTERY_UNKNOWN,
	LL_BATTERY_UNPLUGGED,
	LL_BATTERY_CHARGING, /* on external power, still filling */
	LL_BATTERY_FULL,     /* on external power, full */
};

enum lenslink_thermal_state {
	LL_THERMAL_UNKNOWN,
	LL_THERMAL_NOMINAL,
	LL_THERMAL_FAIR,
	LL_THERMAL_SERIOUS,
	LL_THERMAL_CRITICAL,
};

enum lenslink_system_pressure {
	LL_PRESSURE_UNKNOWN,
	LL_PRESSURE_NOMINAL,
	LL_PRESSURE_FAIR,
	LL_PRESSURE_SERIOUS,
	LL_PRESSURE_CRITICAL,
	LL_PRESSURE_SHUTDOWN, /* iOS has stopped the capture device */
};

/* Bits of lenslink_telemetry.system_pressure_factors. A factor this
 * plugin doesn't know is dropped, not an error. */
enum {
	LL_PRESSURE_FACTOR_SYSTEM_TEMPERATURE = 1u << 0,
	LL_PRESSURE_FACTOR_CAMERA_TEMPERATURE = 1u << 1,
	LL_PRESSURE_FACTOR_DEPTH_MODULE_TEMPERATURE = 1u << 2,
	LL_PRESSURE_FACTOR_PEAK_POWER = 1u << 3,
	LL_PRESSURE_FACTOR_BATTERY_STRESS = 1u << 4,
};

/* The battery level below which an unplugged phone is "low": the same 20%
 * the app's own low-battery tally uses (ios-app BatteryMonitor.lowPercent). */
#define LL_BATTERY_LOW_PERCENT 20

struct lenslink_telemetry {
	bool has_battery_percent;
	int battery_percent; /* 0..100, meaningful only with the flag */
	enum lenslink_battery_state battery_state;
	bool has_low_power_mode;
	bool low_power_mode; /* meaningful only with the flag */
	enum lenslink_thermal_state thermal_state;
	enum lenslink_system_pressure system_pressure;
	uint32_t system_pressure_factors; /* LL_PRESSURE_FACTOR_*; 0 unless
					   * system_pressure is known */
};

/* What is behind an elevated system pressure. */
enum lenslink_pressure_cause {
	LL_PRESSURE_CAUSE_NONE,        /* pressure nominal, fair or unknown */
	LL_PRESSURE_CAUSE_UNSPECIFIED, /* elevated, but no factor reported —
					* say "camera system pressure", never
					* claim heat */
	LL_PRESSURE_CAUSE_THERMAL,     /* a temperature factor */
	LL_PRESSURE_CAUSE_POWER,       /* peak power / battery stress only */
	LL_PRESSURE_CAUSE_THERMAL_AND_POWER,
};

/* How much an operator should care, derived from the facts — not from
 * any colour or wording. */
enum lenslink_telemetry_severity {
	LL_SEVERITY_NORMAL,   /* nothing to say */
	LL_SEVERITY_INFO,     /* Low Power Mode is on */
	LL_SEVERITY_WARNING,  /* battery low and unplugged; thermal or
			       * pressure serious */
	LL_SEVERITY_CRITICAL, /* thermal critical; pressure critical/shutdown */
};

struct lenslink_mic_option {
	char id[96];   /* round-trips to the `mic` command */
	char name[64]; /* display only */
};

struct lenslink_camera_state {
	bool valid; /* a snapshot arrived and parsed */
	uint32_t has;

	/* Pause */
	bool paused;
	enum lenslink_pause_reason pause_reason;

	/* Lens / zoom */
	enum lenslink_camera_position camera;
	char lens[48]; /* wire label, English — translate for display */
	char lenses[LLCS_MAX_LENSES][48];
	size_t lens_count;
	double zoom, max_zoom;

	/* Focus */
	enum lenslink_focus_mode focus_mode;
	double lens_position; /* 0..1 */
	bool face_focus;
	bool supports_face_focus;

	/* Exposure */
	enum lenslink_exposure_mode exposure_mode;
	double exposure_bias; /* EV */
	double iso, min_iso, max_iso;
	double shutter_seconds, min_shutter_seconds, max_shutter_seconds;
	bool supports_manual_exposure;

	/* White balance */
	enum lenslink_wb_mode wb_mode;
	double wb_temperature; /* kelvin */
	bool supports_wb_lock;

	/* Flashlight */
	bool flashlight;
	bool has_flashlight;

	/* Format */
	char resolution[16];
	int fps;
	char codec[16];
	char quality[16];
	char resolutions[LLCS_MAX_RESOLUTIONS][16];
	size_t resolution_count;
	int frame_rates[LLCS_MAX_FRAME_RATES];
	size_t frame_rate_count;
	char codecs[LLCS_MAX_CODECS][16];
	size_t codec_count;

	/* Colour: absent on SDR. */
	bool hdr;       /* HLG */
	char color[16]; /* "log" for Apple Log */

	/* Green screen */
	bool supports_green_screen;
	bool green_screen;
	bool green_screen_depth;
	double green_screen_max_distance; /* metres; 0 = no cutoff */

	/* Phone mic as source audio */
	bool mic_enabled;
	char mic[96];
	struct lenslink_mic_option mics[LLCS_MAX_MICS];
	size_t mic_count;

	/* Phone health */
	struct lenslink_telemetry telemetry;
};

/* Parses one STATE payload (not NUL-terminated; exactly `len` bytes).
 * Returns false and leaves *out cleared (valid == false) on any syntax
 * error or a non-object payload. Unknown keys are ignored. */
bool lenslink_camera_state_parse(const char *json, size_t len,
				 struct lenslink_camera_state *out);

/* Derived, pure classifications of the raw facts (see the enums above). */

/* Any telemetry fact at all is known (false for an app that sends none). */
bool lenslink_telemetry_any(const struct lenslink_telemetry *t);

enum lenslink_pressure_cause
lenslink_telemetry_pressure_cause(const struct lenslink_telemetry *t);

/* Known battery, unplugged (not charging/full), at or below
 * LL_BATTERY_LOW_PERCENT. Unknown plug state is never "low". */
bool lenslink_telemetry_battery_low(const struct lenslink_telemetry *t);

/* The worst of the derived conditions; see enum above. */
enum lenslink_telemetry_severity
lenslink_telemetry_severity(const struct lenslink_telemetry *t);

/* Stable English tokens (the wire spellings) for logs and diagnostics;
 * "unknown" for UNKNOWN. Never for UI text — that is translated. */
const char *lenslink_battery_state_name(enum lenslink_battery_state s);
const char *lenslink_thermal_state_name(enum lenslink_thermal_state s);
const char *lenslink_system_pressure_name(enum lenslink_system_pressure s);
const char *lenslink_pressure_cause_name(enum lenslink_pressure_cause c);

/* Writes `battery=87 charging lpm=0 thermal=nominal pressure=serious
 * factors=cameraTemperature` (unknowns as `?`/`unknown`, `factors=`
 * omitted when none) — the one diagnostic rendering. */
void lenslink_telemetry_format(const struct lenslink_telemetry *t, char *buf,
			       size_t size);

/* A 10-bit colour pipeline is running (HLG or Apple Log). */
static inline bool
lenslink_camera_state_ten_bit(const struct lenslink_camera_state *st)
{
	return st->hdr || (st->has & LLCS_HAS_COLOR);
}

#ifdef __cplusplus
}
#endif
