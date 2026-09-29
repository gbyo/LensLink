#include "rig-model.h"

#include <stdio.h>
#include <string.h>

/* UTF-8 spelled as bytes so no compiler's source-charset guess can
 * corrupt them (MSVC without /utf-8 would). */
#define SEP " \xC2\xB7 "  /* " · " */
#define BOLT "\xE2\x9A\xA1" /* ⚡ */

#define T_(key) (rig->text ? rig->text(key) : (key))

/* --- bounded, UTF-8-safe string building ------------------------------- */

static void cat(char *buf, size_t size, const char *s)
{
	size_t len = strlen(buf);
	if (!s || len + 1 >= size)
		return;
	size_t room = size - len - 1;
	size_t n = strlen(s);
	if (n > room) {
		n = room;
		/* Don't leave half a multi-byte character behind. */
		while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80)
			n--;
	}
	memcpy(buf + len, s, n);
	buf[len + n] = '\0';
}

/* Appends `part`, separated from what is there by " · ". Blank parts are
 * skipped, so a missing figure never leaves "60 fps ·  · USB". */
static void join(char *buf, size_t size, const char *part)
{
	if (!part || !part[0])
		return;
	if (buf[0])
		cat(buf, size, SEP);
	cat(buf, size, part);
}

static void copy(char *dst, size_t size, const char *src)
{
	dst[0] = '\0';
	cat(dst, size, src);
}

/* --- classification ---------------------------------------------------- */

enum lenslink_card_state
lenslink_card_classify(const struct lenslink_source_info *h)
{
	if (h->identity == LENSLINK_IDENTITY_MISMATCH ||
	    h->identity == LENSLINK_IDENTITY_UNIDENTIFIED)
		return CARD_WRONG_DEVICE;
	if (h->tone == STATUS_TONE_ERROR)
		return CARD_ERROR;
	if (!h->connected)
		return h->tone == STATUS_TONE_IDLE ? CARD_IDLE : CARD_OFFLINE;
	if (h->standby)
		return CARD_STANDBY;
	if (h->paused)
		return CARD_PAUSED;
	return h->has_rates ? CARD_LIVE : CARD_CONNECTING;
}

static enum lenslink_attention
attention_from_severity(enum lenslink_telemetry_severity s)
{
	switch (s) {
	case LL_SEVERITY_INFO:
		return ATTENTION_INFO;
	case LL_SEVERITY_WARNING:
		return ATTENTION_WARNING;
	case LL_SEVERITY_CRITICAL:
		return ATTENTION_CRITICAL;
	default:
		return ATTENTION_NONE;
	}
}

static enum lenslink_attention max_att(enum lenslink_attention a,
				       enum lenslink_attention b)
{
	return a > b ? a : b;
}

/* --- text -------------------------------------------------------------- */

static void health_numbers(const struct lenslink_source_info *h, char *buf,
			   size_t size)
{
	char part[48];
	buf[0] = '\0';
	if (h->has_rates) {
		snprintf(part, sizeof(part), "%d fps", (int)(h->fps + 0.5));
		join(buf, size, part);
		snprintf(part, sizeof(part), "%.1f Mb/s", h->bitrate_bps / 1e6);
		join(buf, size, part);
	}
	if (h->latency_ms > 0) {
		snprintf(part, sizeof(part), "%d ms", h->latency_ms);
		join(buf, size, part);
	}
	join(buf, size, h->transport);
}

static void build_health(const struct lenslink_rig *rig,
			 const struct lenslink_source_info *h,
			 enum lenslink_card_state state, char *buf, size_t size)
{
	buf[0] = '\0';
	switch (state) {
	case CARD_LIVE:
		health_numbers(h, buf, size);
		break;
	case CARD_CONNECTING:
		join(buf, size, T_("Dock.State.Connecting"));
		join(buf, size, h->transport);
		break;
	case CARD_STANDBY:
		join(buf, size, T_("Dock.State.Standby"));
		join(buf, size, h->transport);
		break;
	case CARD_PAUSED:
		join(buf, size, T_("Dock.State.Paused"));
		join(buf, size, h->transport);
		break;
	case CARD_OFFLINE:
		join(buf, size, T_("Dock.State.Reconnecting"));
		break;
	case CARD_IDLE:
		/* Already a translated sentence saying what to do. */
		join(buf, size, h->status);
		break;
	case CARD_ERROR:
	case CARD_WRONG_DEVICE:
		if (h->connected)
			join(buf, size, h->transport);
		else
			join(buf, size, T_("Dock.State.Reconnecting"));
		break;
	}
}

static void build_battery(const struct lenslink_source_info *h,
			  struct lenslink_card *c)
{
	c->battery[0] = '\0';
	const struct lenslink_telemetry *t = &h->telemetry;
	if (!h->connected)
		return; /* telemetry is dropped with the connection; never
			 * present a dead phone's last figure as current */

	c->charging = t->battery_state == LL_BATTERY_CHARGING ||
		      t->battery_state == LL_BATTERY_FULL;
	if (t->has_battery_percent)
		snprintf(c->battery, sizeof(c->battery), "%d%%%s",
			 t->battery_percent, c->charging ? " " BOLT : "");
	else if (c->charging)
		snprintf(c->battery, sizeof(c->battery), BOLT);
}

/* One candidate condition, in operator-priority order. */
struct condition {
	char text[128]; /* "" = no line of its own (shown as the health) */
	enum lenslink_attention att;
};

static const char *pressure_cause_text(const struct lenslink_rig *rig,
				       const struct lenslink_telemetry *t)
{
	switch (lenslink_telemetry_pressure_cause(t)) {
	case LL_PRESSURE_CAUSE_THERMAL:
		return T_("Dock.Warn.Pressure.Thermal");
	case LL_PRESSURE_CAUSE_POWER:
		return T_("Dock.Warn.Pressure.Power");
	case LL_PRESSURE_CAUSE_THERMAL_AND_POWER:
		return T_("Dock.Warn.Pressure.Both");
	default:
		return T_("Dock.Warn.Pressure.System");
	}
}

/* The priority list from the design (highest actionability first):
 *  1 wrong device      2 connection error   3 disconnected
 *  4 pressure shutdown 5 pressure critical  6 device thermal critical
 *  7 pressure serious  8 device thermal serious
 *  9 low unplugged battery                 10 Low Power Mode (info)
 * `conds` gets every condition that holds, in that order; the card's
 * warning line is the first one that is a warning, its attention the worst
 * of them all. */
static size_t collect_conditions(const struct lenslink_rig *rig,
				 const struct lenslink_source_info *h,
				 enum lenslink_card_state state,
				 struct condition *conds)
{
	size_t n = 0;
#define ADD(str, level)                                          \
	do {                                                     \
		copy(conds[n].text, sizeof(conds[n].text), (str)); \
		conds[n].att = (level);                          \
		n++;                                             \
	} while (0)
	const struct lenslink_telemetry *t = &h->telemetry;

	if (state == CARD_WRONG_DEVICE)
		ADD(h->identity == LENSLINK_IDENTITY_MISMATCH
			    ? T_("Dock.Warn.WrongDevice")
			    : T_("Dock.Warn.Unidentified"),
		    ATTENTION_WARNING);
	if (state == CARD_ERROR)
		ADD(h->connected ? T_("Dock.Warn.Stream")
				 : T_("Dock.Warn.Connection"),
		    ATTENTION_WARNING);
	if (state == CARD_OFFLINE || state == CARD_WRONG_DEVICE ||
	    (state == CARD_ERROR && !h->connected))
		ADD("", ATTENTION_WARNING); /* the health line says it */

	if (h->connected) {
		char text[128];

		if (t->system_pressure == LL_PRESSURE_SHUTDOWN)
			ADD(T_("Dock.Warn.Shutdown"), ATTENTION_CRITICAL);
		if (t->system_pressure == LL_PRESSURE_CRITICAL) {
			copy(text, sizeof(text), pressure_cause_text(rig, t));
			join(text, sizeof(text), T_("Dock.Level.Critical"));
			ADD(text, ATTENTION_CRITICAL);
		}
		if (t->thermal_state == LL_THERMAL_CRITICAL) {
			copy(text, sizeof(text), T_("Dock.Warn.DeviceHot"));
			join(text, sizeof(text), T_("Dock.Level.Critical"));
			ADD(text, ATTENTION_CRITICAL);
		}
		if (t->system_pressure == LL_PRESSURE_SERIOUS)
			ADD(pressure_cause_text(rig, t), ATTENTION_WARNING);
		if (t->thermal_state == LL_THERMAL_SERIOUS)
			ADD(T_("Dock.Warn.DeviceHot"), ATTENTION_WARNING);
		if (lenslink_telemetry_battery_low(t))
			ADD(T_("Dock.Warn.LowBattery"), ATTENTION_WARNING);
		if (t->has_low_power_mode && t->low_power_mode)
			ADD(T_("Dock.Warn.LowPower"), ATTENTION_INFO);
	}
#undef ADD
	return n;
}

static void build_texts(const struct lenslink_rig *rig,
			const struct lenslink_source_info *h,
			struct lenslink_card *c)
{
	build_health(rig, h, c->state, c->health, sizeof(c->health));
	build_battery(h, c);
	c->battery_low = h->connected &&
			 lenslink_telemetry_battery_low(&h->telemetry);

	struct condition conds[8];
	size_t n = collect_conditions(rig, h, c->state, conds);

	c->attention = ATTENTION_NONE;
	c->warning[0] = c->note[0] = '\0';
	for (size_t i = 0; i < n; i++) {
		c->attention = max_att(c->attention, conds[i].att);
		if (conds[i].att >= ATTENTION_WARNING && conds[i].text[0] &&
		    !c->warning[0])
			copy(c->warning, sizeof(c->warning), conds[i].text);
		if (conds[i].att == ATTENTION_INFO && !c->note[0])
			copy(c->note, sizeof(c->note), conds[i].text);
	}
	/* The Stage 3 severity is the authority on the telemetry half. */
	if (h->connected)
		c->attention = max_att(
			c->attention,
			attention_from_severity(
				lenslink_telemetry_severity(&h->telemetry)));
	/* A quieter line never appears beside a stronger one. */
	if (c->warning[0] || c->attention >= ATTENTION_WARNING)
		c->note[0] = '\0';

	/* Tooltip: everything, one item per line. */
	char *tip = c->tooltip;
	size_t tsz = sizeof(c->tooltip);
	tip[0] = '\0';
	cat(tip, tsz, c->name);
	if (h->device_name[0]) {
		cat(tip, tsz, " " SEP);
		cat(tip, tsz, h->device_name);
	}
	if (c->program || c->preview) {
		cat(tip, tsz, "\n");
		if (c->program)
			cat(tip, tsz, T_("Dock.OnAir"));
		if (c->program && c->preview)
			cat(tip, tsz, SEP);
		if (c->preview)
			cat(tip, tsz, T_("Dock.InPreview"));
	}
	if (h->status[0]) {
		cat(tip, tsz, "\n");
		cat(tip, tsz, h->status);
	}
	for (size_t i = 0; i < n; i++) {
		if (conds[i].text[0]) {
			cat(tip, tsz, "\n");
			cat(tip, tsz, conds[i].text);
		}
	}
	if (c->battery[0]) {
		cat(tip, tsz, "\n");
		cat(tip, tsz, T_("Dock.Battery"));
		cat(tip, tsz, " ");
		cat(tip, tsz, c->battery);
	}

	/* Accessible description: the state without relying on colour. */
	char *a = c->accessible;
	size_t asz = sizeof(c->accessible);
	a[0] = '\0';
	if (!c->is_screen) {
		char num[16];
		snprintf(num, sizeof(num), " %d", c->number);
		join(a, asz, T_("Dock.A11y.Camera"));
		cat(a, asz, num);
	} else {
		join(a, asz, T_("Dock.A11y.Screen"));
	}
	join(a, asz, c->name);
	if (c->program)
		join(a, asz, T_("Dock.OnAir"));
	if (c->preview)
		join(a, asz, T_("Dock.InPreview"));
	if (c->selected)
		join(a, asz, T_("Dock.Selected"));
	join(a, asz, c->health);
	join(a, asz, c->warning);
	join(a, asz, c->note);
	if (c->battery[0]) {
		char b[64];
		b[0] = '\0';
		cat(b, sizeof(b), T_("Dock.Battery"));
		cat(b, sizeof(b), " ");
		cat(b, sizeof(b), c->battery);
		join(a, asz, b);
		if (c->charging)
			join(a, asz, T_("Dock.Charging"));
	}
}

/* --- build ------------------------------------------------------------- */

void lenslink_rig_init(struct lenslink_rig *rig, lenslink_text_fn text)
{
	memset(rig, 0, sizeof(*rig));
	rig->text = text;
}

void lenslink_rig_reset(struct lenslink_rig *rig)
{
	lenslink_rig_order_clear(&rig->order);
	rig->selected[0] = '\0';
	rig->selected_slot = 0;
}

static bool uuid_in_set(const struct lenslink_uuid_set *set, const char *uuid)
{
	return set && lenslink_uuid_set_has(set, uuid);
}

static void fill_card(const struct lenslink_source_info *h,
		      const struct lenslink_uuid_set *preview,
		      struct lenslink_card *c)
{
	memset(c, 0, sizeof(*c));
	c->id = h->id;
	copy(c->uuid, sizeof(c->uuid), h->uuid);
	c->is_screen = h->is_screen;
	copy(c->name, sizeof(c->name), h->source_name);
	copy(c->device_name, sizeof(c->device_name), h->device_name);

	/* Two independent sources of truth; `h->preview` is deliberately
	 * not consulted (see the header). */
	c->program = h->program;
	c->preview = uuid_in_set(preview, h->uuid);

	c->state = lenslink_card_classify(h);
	c->connected = h->connected;
	c->can_start = h->connected && c->state == CARD_STANDBY;
	c->can_pause = !h->is_screen &&
		       (c->state == CARD_LIVE || c->state == CARD_CONNECTING);
	c->can_resume = !h->is_screen && c->state == CARD_PAUSED;
	c->can_stop = !h->is_screen &&
		      (c->state == CARD_LIVE || c->state == CARD_CONNECTING ||
		       c->state == CARD_PAUSED);
	c->can_recalibrate = !h->is_screen && h->connected &&
			     c->state != CARD_STANDBY;
}

static void build_summary(const struct lenslink_rig *rig,
			  struct lenslink_rig_view *v)
{
	struct lenslink_rig_summary *s = &v->summary;
	memset(s, 0, sizeof(*s));
	for (size_t i = 0; i < v->count; i++) {
		const struct lenslink_card *c = &v->cards[i];
		if (c->is_screen) {
			s->screens++;
			continue;
		}
		s->cameras++;
		if (c->state == CARD_LIVE)
			s->live++;
		if (c->attention >= ATTENTION_WARNING)
			s->attention++;
	}

	/* Bitrate is summed from the live cameras' own figures: a
	 * disconnected camera's last rate is stale and never counted. It is
	 * stream throughput, not network capacity or utilisation. */
	for (size_t i = 0; i < v->camera_count; i++) {
		const struct lenslink_card *c = &v->cards[i];
		if (c->state == CARD_LIVE)
			s->bitrate_bps += c->bitrate_bps;
	}

	char num[32];
	s->headline[0] = s->status[0] = '\0';
	if (s->cameras == 0) {
		copy(s->headline, sizeof(s->headline), T_("Dock.Summary.None"));
		return;
	}
	snprintf(num, sizeof(num), "%d ", s->cameras);
	cat(s->headline, sizeof(s->headline), num);
	cat(s->headline, sizeof(s->headline),
	    s->cameras == 1 ? T_("Dock.Summary.Camera.One")
			    : T_("Dock.Summary.Camera.Other"));
	snprintf(num, sizeof(num), "%d ", s->live);
	char part[64];
	copy(part, sizeof(part), num);
	cat(part, sizeof(part), T_("Dock.Summary.Live"));
	join(s->headline, sizeof(s->headline), part);
	if (s->live > 0) {
		snprintf(part, sizeof(part), "%.1f Mb/s",
			 s->bitrate_bps / 1e6);
		join(s->headline, sizeof(s->headline), part);
	}

	if (s->attention == 0) {
		copy(s->status, sizeof(s->status), T_("Dock.Summary.Healthy"));
	} else {
		snprintf(num, sizeof(num), "%d ", s->attention);
		copy(s->status, sizeof(s->status), num);
		cat(s->status, sizeof(s->status),
		    s->attention == 1 ? T_("Dock.Summary.Attention.One")
				      : T_("Dock.Summary.Attention.Other"));
	}
}

void lenslink_rig_build(struct lenslink_rig *rig,
			const struct lenslink_source_info *sources, size_t n,
			const struct lenslink_uuid_set *preview,
			struct lenslink_rig_view *out)
{
	memset(out, 0, sizeof(*out));

	/* Only sources that can be placed: those with a UUID. */
	const struct lenslink_source_info *usable[LENSLINK_MAX_SOURCES];
	const char *keys[LENSLINK_MAX_SOURCES];
	size_t usable_count = 0;
	for (size_t i = 0; i < n && usable_count < LENSLINK_MAX_SOURCES; i++) {
		if (!sources[i].uuid[0])
			continue;
		bool dup = false;
		for (size_t k = 0; k < usable_count; k++)
			if (strcmp(keys[k], sources[i].uuid) == 0)
				dup = true;
		if (dup)
			continue;
		usable[usable_count] = &sources[i];
		keys[usable_count] = sources[i].uuid;
		usable_count++;
	}

	/* One order covers both kinds; the dock shows cameras first, then
	 * screens, each in that order. */
	size_t slot[LENSLINK_MAX_SOURCES];
	size_t slots = lenslink_rig_order_arrange(&rig->order, keys,
						  usable_count, slot);

	for (int pass = 0; pass < 2; pass++) {
		bool want_screens = pass == 1;
		for (size_t k = 0; k < slots; k++) {
			const struct lenslink_source_info *h = usable[slot[k]];
			if (h->is_screen != want_screens)
				continue;
			struct lenslink_card *c = &out->cards[out->count++];
			fill_card(h, preview, c);
			if (!want_screens)
				c->number = (int)++out->camera_count;
		}
	}

	/* Selection: cameras only, by UUID. A vanished selection falls to
	 * whoever now holds its slot (else the last camera, else none). */
	if (rig->selected[0]) {
		bool found = false;
		for (size_t i = 0; i < out->camera_count; i++) {
			if (strcmp(out->cards[i].uuid, rig->selected) == 0) {
				rig->selected_slot = i;
				found = true;
				break;
			}
		}
		if (!found) {
			if (out->camera_count == 0) {
				rig->selected[0] = '\0';
				rig->selected_slot = 0;
			} else {
				size_t at = rig->selected_slot;
				if (at >= out->camera_count)
					at = out->camera_count - 1;
				copy(rig->selected, sizeof(rig->selected),
				     out->cards[at].uuid);
				rig->selected_slot = at;
			}
		}
	}

	for (size_t i = 0; i < out->count; i++) {
		struct lenslink_card *c = &out->cards[i];
		const struct lenslink_source_info *h = NULL;
		for (size_t k = 0; k < usable_count; k++)
			if (strcmp(usable[k]->uuid, c->uuid) == 0)
				h = usable[k];
		c->selected = !c->is_screen && rig->selected[0] &&
			      strcmp(c->uuid, rig->selected) == 0;
		c->bitrate_bps = h->has_rates ? h->bitrate_bps : 0;
		build_texts(rig, h, c);
	}

	build_summary(rig, out);
}

bool lenslink_rig_select(struct lenslink_rig *rig,
			 const struct lenslink_rig_view *view,
			 const char *uuid)
{
	if (!uuid || !uuid[0])
		return false;
	for (size_t i = 0; i < view->camera_count; i++) {
		if (strcmp(view->cards[i].uuid, uuid) == 0) {
			copy(rig->selected, sizeof(rig->selected), uuid);
			rig->selected_slot = i;
			return true;
		}
	}
	return false;
}
