/*
 * The dock's brain, with no widgets in it: registry snapshot in, ready-to-
 * show camera cards and a rig summary out.
 *
 * Three facts about a camera are kept deliberately independent — in the
 * inputs, in the code and in the result — because an operator's picture of
 * the show depends on never confusing them:
 *
 *   PROGRAM   the source is contributing to OBS's final mix. It is
 *             `lenslink_source_info.program`, which mirrors libobs's own
 *             source-active state (the source's activate / deactivate
 *             callbacks) — not the current scene's name, and it is true
 *             for both cameras while a transition between them runs.
 *   PREVIEW   the source is visible through the Studio Mode Preview
 *             scene: membership of the `preview` set, produced by
 *             scene-walk.h. Studio Mode off => the set is empty. The
 *             registry's `preview` flag ("showing but not program",
 *             which a projector satisfies) is IGNORED here.
 *   SELECTED  the camera the operator picked for LensLink control
 *             (rig->selected). Choosing it never touches OBS: it
 *             is dock-local state, and neither of the other two can
 *             change it or be changed by it.
 *
 * Slots and numbers come from rig-order.h; the wording and severity of
 * warnings from the Stage 3 telemetry helpers in camera-state.h (system
 * pressure is not thermal state, Low Power Mode is information only, a
 * charging phone is never "low"). All user-visible text comes through the
 * caller's lookup function (obs_module_text in the plugin) and is
 * concatenated, never used as a format.
 *
 * Plain C, no libobs, no Qt: unit-tested in tests/test-rig-model.c.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rig-order.h"
#include "scene-walk.h"
#include "source-registry.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef const char *(*lenslink_text_fn)(const char *key);

/* What a card is doing, from the registry's own facts (no state machine of
 * the dock's own). */
enum lenslink_card_state {
	CARD_LIVE,         /* connected, video flowing */
	CARD_CONNECTING,   /* connected, no video rates yet */
	CARD_STANDBY,      /* connected, camera idle (remote start) */
	CARD_PAUSED,       /* connected, picture held */
	CARD_OFFLINE,      /* not connected; the source is dialling */
	CARD_IDLE,         /* not connected and nothing to do (no host, or
			    * hidden and set to disconnect) */
	CARD_ERROR,        /* the source reports an error */
	CARD_WRONG_DEVICE, /* a phone answered but isn't the bound one */
};

enum lenslink_attention {
	ATTENTION_NONE,
	ATTENTION_INFO,     /* worth a subtle line, not a warning */
	ATTENTION_WARNING,
	ATTENTION_CRITICAL,
};

struct lenslink_card {
	/* Addressing only: the runtime id is opaque and session-local. */
	uint32_t id;
	/* The identity everything else keys on (order, selection, widgets). */
	char uuid[LENSLINK_UUID_SIZE];

	bool is_screen;
	int number; /* 1-based slot among cameras; 0 for a screen */
	char name[128];        /* the OBS source name — the production role */
	char device_name[64];  /* the phone's own name; secondary */

	bool program;
	bool preview;
	bool selected;

	enum lenslink_card_state state;
	enum lenslink_attention attention;
	bool connected;
	bool charging;
	bool battery_low;

	double bitrate_bps; /* current stream rate; 0 unless rates are valid */

	char health[192];  /* "60 fps · 9.8 Mb/s · 45 ms · Wi-Fi" or a state */
	char battery[24];  /* "81%", "92% ⚡", or "" when unknown */
	char warning[128]; /* the one line worth the operator's eye, or "" */
	char note[64];     /* a quieter informational line (Low Power Mode) */
	char tooltip[1024];
	char accessible[512];

	/* Which safe actions the card's menu offers right now. */
	bool can_start, can_pause, can_resume, can_stop, can_recalibrate;
};

struct lenslink_rig_summary {
	int cameras;    /* LensLink Camera sources (screens excluded) */
	int screens;
	int live;       /* cameras in CARD_LIVE */
	int attention;  /* cameras at WARNING or above */
	double bitrate_bps; /* sum over LIVE cameras: throughput, not capacity */
	char headline[128]; /* "4 cameras · 3 live · 38.7 Mb/s" */
	char status[96];    /* "All healthy" / "1 camera needs attention", or
			     * "" when there are no cameras */
};

struct lenslink_rig_view {
	/* Cameras in slot order first, then screens in order. */
	struct lenslink_card cards[LENSLINK_MAX_SOURCES];
	size_t count;
	size_t camera_count;
	struct lenslink_rig_summary summary;
};

struct lenslink_rig {
	struct lenslink_rig_order order;
	char selected[LENSLINK_UUID_SIZE]; /* "" = none */
	size_t selected_slot;              /* where it last sat (fallback) */
	lenslink_text_fn text;
};

void lenslink_rig_init(struct lenslink_rig *rig, lenslink_text_fn text);

/* New scene collection: forget the order and the selection. */
void lenslink_rig_reset(struct lenslink_rig *rig);

/* Builds the view. `preview` may be NULL (= Studio Mode off / unknown:
 * nothing is PVW). Sources whose uuid is empty cannot be placed and are
 * skipped. Reconciles the order (new cameras append) and the selection
 * (a selected camera that vanished hands the selection to whoever now
 * holds its slot, else the last camera, else none). */
void lenslink_rig_build(struct lenslink_rig *rig,
			const struct lenslink_source_info *sources, size_t n,
			const struct lenslink_uuid_set *preview,
			struct lenslink_rig_view *out);

/* Selects `uuid` for LensLink control. Only a camera in `view` can be
 * selected. Changes nothing else, by construction. */
bool lenslink_rig_select(struct lenslink_rig *rig,
			 const struct lenslink_rig_view *view,
			 const char *uuid);

/* Exposed for tests. */
enum lenslink_card_state
lenslink_card_classify(const struct lenslink_source_info *info);

#ifdef __cplusplus
}
#endif
