/*
 * The one registry of live LensLink sources, and the one API every
 * surface uses to read and operate them.
 *
 *                 iPhone STATE
 *                      |
 *              ios-camera-source.c   (owns the private source struct)
 *                      |
 *             source-registry.c      (ids, lifetime, snapshots, commands)
 *            /         |          \
 *     Qt frontend   web panel   diagnostics
 *
 * Consumers never hold a `struct ios_camera_source *`. They address a
 * source by its runtime id — a small integer assigned at creation, unique
 * for the life of the OBS process and never reused — and every call
 * resolves the id under the registry lock. Runtime ids are opaque and
 * session-local: never persisted, never meaningful across OBS launches,
 * and unrelated to a phone's persistent deviceID (device-identity.h),
 * which is what a source is *bound* to. They are uint32_t, so they stay
 * exact in JavaScript (below 2^53) wherever the web API exposes them. A source unregisters first
 * thing in destroy, so once that returns no call can reach it; a call
 * for a gone id simply fails. That is the whole lifetime contract, and it
 * is the same one the web panel and the health snapshots each used to
 * implement separately.
 *
 * All functions here are thread-safe and cheap (a lock and a copy). None
 * blocks on the network or on the dial loop: commands are queued for the
 * source's dial loop to send, like the web panel always did.
 *
 * Plain C with a C++ guard: frontend-ui.cpp includes this, and nothing
 * else from the source's internals.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "camera-state.h"
#include "device-identity.h"
#include "lenslink-uuid.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LENSLINK_MAX_SOURCES 32

/* The colour/meaning of a source's status line (docs/UI_DESIGN.md
 * vocabulary): recorded with every status change so no surface ever
 * derives colour from the (translated) text. */
enum ios_camera_status_tone {
	STATUS_TONE_IDLE,
	STATUS_TONE_WAIT,
	STATUS_TONE_READY,
	STATUS_TONE_LIVE,
	STATUS_TONE_ERROR,
};

/* Everything about one source that a list/dock/diagnostics row needs —
 * but not the camera STATE, which is fetched separately (and only for the
 * source that needs it) with lenslink_source_get_camera_state. */
struct lenslink_source_info {
	uint32_t id;            /* runtime id; never 0 for a real source */
	char source_name[128];  /* the OBS source's (user-editable) name —
				 * the production role, e.g. "Stage Left" */
	char uuid[LENSLINK_UUID_SIZE]; /* the OBS source's persistent UUID:
				 * the key for anything saved or selected
				 * across renames and restarts (the runtime
				 * id above is neither) */
	char device_name[64];   /* connected phone's display name (HELLO);
				 * "" if unknown. Changes when the phone is
				 * renamed — display only, never identity. */

	/* Persistent identity (device-identity.h). `device_id` is the
	 * phone this source is bound to, "" for an unbound (legacy) source
	 * that accepts whatever answers at its address ("INVALID" if the
	 * saved setting is malformed: bound, matching no phone — it fails
	 * closed). `peer_device_id` is
	 * what the phone on the current connection announced — or, after an
	 * identity rejection, what the rejected phone announced ("" = none:
	 * an older app, or nothing connected). `resolved_host` is the Wi-Fi
	 * address last dialed ("" for USB, or before the first dial). Raw
	 * ids are for diagnostics, not for normal UI text. */
	char device_id[LENSLINK_DEVICE_ID_SIZE];
	char peer_device_id[LENSLINK_DEVICE_ID_SIZE];
	char resolved_host[128];
	enum lenslink_identity_state identity;
	char status[1024];      /* the source's status line (translated) */
	enum ios_camera_status_tone tone;

	bool is_screen;         /* LensLink Screen source type (vs Camera) */
	bool stream_is_screen;  /* the live connection is a screen mirror */
	bool connected;         /* a device connection is live */
	bool standby;           /* connected, camera idle (remote start) */
	bool paused;            /* connected, video held (STATE paused) */
	bool has_state;         /* a STATE arrived this connection (in
				 * standby it carries telemetry only) */
	bool auto_start;        /* the source's auto-start property */
	const char *sync;       /* lip-sync stage: "off"/"measuring"/
				 * "locked"/"relocking" (static string) */

	/* OBS tally, exactly as sent to the phone. `program`: part of what
	 * OBS outputs — libobs's own source-active state, so it is true
	 * through nested scenes and for both sides of a running transition.
	 * `preview`: shown somewhere (studio preview, a projector, a
	 * rendered-but-hidden scene) but not on program — a source-level
	 * approximation that is NOT OBS Preview; only the phone's own tally
	 * still uses it. The dock ignores it: true Preview needs the
	 * frontend's scene knowledge (scene-walk.h). */
	bool program;
	bool preview;

	char transport[16];     /* "USB" / "Wi-Fi" */
	bool gpu_pipeline;      /* this source is on the zero-copy path */
	bool green_screen;      /* the phone is painting its background */
	int last_dial_error;    /* errno/WSA code of the last failed dial */

	/* Cumulative per-connection counters, mirrored ~1 Hz by the dial
	 * loop; see diagnostics.c for how their relationship diagnoses a
	 * stall. */
	uint64_t frames;        /* decoded frames */
	uint64_t bytes;         /* wire video bytes */
	uint64_t video_packets;
	uint64_t keyframes;
	uint64_t decode_errors;
	int hw_retries;         /* hardware decode fell back this often */
	char decoder[32];       /* "VideoToolbox", "D3D11VA", "software", … */
	int latency_ms;         /* avg capture→decode; 0 until measured */

	/* The phone's power and capture health from its latest STATE — the
	 * facts only (camera-state.h has the classification helpers and the
	 * warning that system pressure is not thermal state). All unknown
	 * until a STATE with telemetry arrives on this connection; cleared
	 * with the connection. Carried here, beside the row's other
	 * fields, so a list or dock needs no second fetch and no JSON. */
	struct lenslink_telemetry telemetry;

	/* Rates derived by the dial loop from successive 1 Hz mirrors, so
	 * every surface shows the same numbers. Valid only when has_rates
	 * (false until two samples of one decode stream exist). */
	bool has_rates;
	double fps;
	double bitrate_bps;
};

/* --- Enumeration / lookup ------------------------------------------ */

/* Snapshots every live source, in creation order; returns the count (at
 * most `max`). */
size_t lenslink_source_enum(struct lenslink_source_info *out, size_t max);

/* One source by id. False if the id is gone. */
bool lenslink_source_get_info(uint32_t id, struct lenslink_source_info *out);

/* The source's typed camera STATE (valid == false until a snapshot
 * arrives on the current connection). False if the id is gone. */
bool lenslink_source_get_camera_state(uint32_t id,
				      struct lenslink_camera_state *out);

/* The raw STATE JSON exactly as last received ("{}" when none), for
 * protocol-level consumers such as /api/state. False if the id is gone.
 * Truncates to `size`; the cache itself holds 2048 bytes. */
bool lenslink_source_copy_state_json(uint32_t id, char *buf, size_t size);

/* A camera (not screen) source to address: `id` itself when it names a
 * live camera source, otherwise the first camera source, otherwise 0.
 * This is the web panel's historical ?src= fallback. */
uint32_t lenslink_source_resolve_camera(uint32_t id);

/* Number of live LensLink Camera sources (screen sources excluded). */
size_t lenslink_source_camera_count(void);

/* A new strong reference to the source's obs_source_t (for rendering
 * or opening its properties), or NULL if the id is gone or the source is
 * already being destroyed. The caller must obs_source_release() it.
 * Declared with an opaque type so this header needs no libobs. */
struct obs_source;
struct obs_source *lenslink_source_get_obs_source(uint32_t id);

/* --- Control ---------------------------------------------------------- */

/* Queues one CONTROL JSON command (docs/PROTOCOL.md §7) for the phone.
 * Silently dropped by the dial loop when nothing is connected. False if
 * the id is gone. */
bool lenslink_source_send_control(uint32_t id, const char *json, size_t len);

enum lenslink_stream_command {
	LENSLINK_STREAM_START,  /* start_stream (remote start) */
	LENSLINK_STREAM_STOP,   /* stop_stream */
	LENSLINK_STREAM_PAUSE,  /* pause_stream (hold the picture) */
	LENSLINK_STREAM_RESUME, /* resume_stream */
};

/* The canonical CONTROL payload for a stream command. */
const char *lenslink_stream_command_json(enum lenslink_stream_command cmd);

bool lenslink_source_stream_command(uint32_t id,
				    enum lenslink_stream_command cmd);

/* Persists the source's auto-start property (the same value the
 * properties checkbox edits). */
bool lenslink_source_set_auto_start(uint32_t id, bool on);

/* Drops the locked lip-sync mic figure and measures afresh. */
bool lenslink_source_recalibrate(uint32_t id);

/* Binds the source to the phone with persistent `device_id` (any case;
 * stored canonical), or unbinds it with NULL/"". Persisted in the
 * source's settings; the source reconnects, and from then on accepts
 * only a phone whose HELLO proves that id. This is an explicit
 * operation for a setup flow to call after the user picked a phone —
 * nothing binds a source implicitly. False if the id is gone or
 * `device_id` is not a valid UUID. */
bool lenslink_source_bind_device(uint32_t id, const char *device_id);

/* ===================================================================== */
/* Provider side — for ios-camera-source.c only.                          */
/* ===================================================================== */

struct ios_camera_source;

/* Called at the end of create / first thing in destroy. unregister
 * blocks until any in-flight registry call on the source has finished;
 * after it returns the registry never touches the source again. Returns
 * the assigned id (0 if the registry is full — the source then works
 * but no surface can see it, as before with the old fixed tables). */
uint32_t lenslink_registry_add(struct ios_camera_source *s);
void lenslink_registry_remove(struct ios_camera_source *s);

/* Runs `fn` for every live source with the registry locked (so sources
 * can't be destroyed meanwhile). `fn` must not call back into this API. */
void lenslink_registry_for_each(void (*fn)(struct ios_camera_source *s,
					   void *param),
				void *param);

/* Implemented by ios-camera-source.c. Each is called with the registry
 * lock held and must only take the source's own locks. */
void ios_camera_fill_info(struct ios_camera_source *s,
			  struct lenslink_source_info *out);
bool ios_camera_is_screen_source(struct ios_camera_source *s);
void ios_camera_copy_camera_state(struct ios_camera_source *s,
				  struct lenslink_camera_state *out);
void ios_camera_copy_state(struct ios_camera_source *s, char *buf,
			   size_t size);
struct obs_source *ios_camera_obs_source(struct ios_camera_source *s);
void ios_camera_enqueue_control(struct ios_camera_source *s, const char *json,
				size_t len);
void ios_camera_set_auto_start(struct ios_camera_source *s, bool on);
void ios_camera_recalibrate(struct ios_camera_source *s);
/* `device_id` is canonical, or "" to unbind. */
void ios_camera_bind_device(struct ios_camera_source *s,
			    const char *device_id);

#ifdef __cplusplus
}
#endif
