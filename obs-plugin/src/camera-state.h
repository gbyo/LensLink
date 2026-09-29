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
};

/* Parses one STATE payload (not NUL-terminated; exactly `len` bytes).
 * Returns false and leaves *out cleared (valid == false) on any syntax
 * error or a non-object payload. Unknown keys are ignored. */
bool lenslink_camera_state_parse(const char *json, size_t len,
				 struct lenslink_camera_state *out);

/* A 10-bit colour pipeline is running (HLG or Apple Log). */
static inline bool
lenslink_camera_state_ten_bit(const struct lenslink_camera_state *st)
{
	return st->hdr || (st->has & LLCS_HAS_COLOR);
}

#ifdef __cplusplus
}
#endif
