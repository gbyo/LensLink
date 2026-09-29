/*
 * Frontend UI: a live health readout in OBS's status bar (fps · Mb/s ·
 * latency, next to OBS's own stats) plus the LensLink dock — a compact
 * strip of camera cards (rig-dock.cpp) driven by the rig model
 * (rig-model.c) from the snapshots ios-camera-source.c keeps anyway.
 *
 * Deliberate choices that keep this portable and optional:
 *
 * - The obs-frontend-api symbols are resolved AT RUNTIME from the running
 *   process (dlsym / GetProcAddress). The plugin never links the frontend
 *   library — the same binary loads in OBS builds without a frontend
 *   (headless, older) and the UI just stays off. Only the header is
 *   needed at compile time, for the types. Every symbol beyond the three
 *   the UI can't exist without is optional: without the Studio Mode calls
 *   nothing is ever PVW, without save callbacks the order isn't
 *   persisted, and so on — never a crash at module load.
 * - No Q_OBJECT: timers and cross-thread posts bind lambdas, so no moc,
 *   and the plugin's plain CMake stays plain.
 * - This layer owns only wiring. What the operator sees is decided by
 *   rig-model.c (pure, tested); how it is drawn is rig-dock.cpp; which
 *   sources are in OBS Preview is scene-walk-obs.c.
 *
 * Everything here runs on the Qt UI thread. libobs signal callbacks can
 * arrive on any thread (transitions activate sources on the graphics
 * thread), so they only *post* a coalesced refresh to the UI thread; they
 * never touch a widget. State comes from the shared source registry
 * (source-registry.h), which locks internally and addresses sources by id
 * — never a pointer into a source — and from the frontend's Preview scene
 * queried afresh on every refresh; no OBS pointer is kept between calls.
 *
 * Refresh cadence: the numbers (fps, bitrate, latency, battery) ride the
 * existing 1 s timer. Program/Preview/collection changes don't wait for
 * it: frontend events and libobs's source signals request a refresh at
 * once. The timer also re-derives Preview, which is what notices a scene
 * item being hidden inside the Preview scene (no event announces that).
 */

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/platform.h>

#include <QCheckBox>
#include <QClipboard>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGuiApplication>
#include <QLabel>
#include <QMainWindow>
#include <QMetaObject>
#include <QPushButton>
#include <QSpinBox>
#include <QStatusBar>
#include <QStringList>
#include <QTimer>
#include <QVBoxLayout>

#include <atomic>
#include <memory>
#include <vector>

#include "rig-dock.h"

extern "C" {
#include "diagnostics.h"
#include "rig-model.h"
#include "scene-walk-obs.h"
#include "source-registry.h"
#include "plugin-settings.h"
void lenslink_frontend_init(void);
void lenslink_frontend_shutdown(void);
}

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace {

constexpr char DOCK_ID[] = "lenslink_health";

/* Where this module keeps its data inside the scene collection's
 * "modules" object (save callbacks): modules.lenslink.dock.order. */
constexpr char SAVE_KEY[] = "lenslink";
constexpr char SAVE_DOCK[] = "dock";
constexpr char SAVE_ORDER[] = "order";
constexpr int SAVE_VERSION = 1;

/* Runtime-resolved frontend API (see file comment). */
struct FrontendApi {
	void (*add_event_callback)(obs_frontend_event_cb, void *);
	void (*remove_event_callback)(obs_frontend_event_cb, void *);
	void *(*get_main_window)(void);
	bool (*add_dock_by_id)(const char *, const char *, void *);
	void (*remove_dock)(const char *);
	void (*add_tools_menu_item)(const char *, obs_frontend_cb, void *);
	/* Optional: each only gates the feature that needs it. */
	bool (*preview_program_mode_active)(void);
	obs_source_t *(*get_current_preview_scene)(void);
	void (*add_save_callback)(obs_frontend_save_cb, void *);
	void (*remove_save_callback)(obs_frontend_save_cb, void *);
	void (*open_source_properties)(obs_source_t *);
};

FrontendApi api = {};
bool callback_installed = false;
bool save_callback_installed = false;

template<typename T> void resolve(T &fn, const char *name)
{
#ifdef _WIN32
	HMODULE module = GetModuleHandleA("obs-frontend-api.dll");
	fn = module ? reinterpret_cast<T>(
			      GetProcAddress(module, name))
		    : nullptr;
#else
	fn = reinterpret_cast<T>(dlsym(RTLD_DEFAULT, name));
#endif
}

QLabel *status_label = nullptr;
lenslink::RigDock *dock = nullptr; /* owned by OBS once the dock is added */
QTimer *timer = nullptr;

/* The dock's whole state, all of it copies and UUID strings: nothing here
 * points into OBS. Reset wholesale when the scene collection changes. */
lenslink_rig rig;
lenslink_uuid_set preview_set;
bool studio_mode = false;
bool collection_busy = false; /* between CHANGING and CHANGED: no queries */
std::unique_ptr<lenslink_rig_view> view;
std::vector<lenslink_source_info> snapshot;
size_t snapshot_count = 0;

/* Cross-thread refresh requests (libobs signals): coalesced into one
 * queued call on the UI thread. `post_target` is nulled before the
 * signals are disconnected. */
std::atomic<bool> refresh_pending{false};
std::atomic<QObject *> post_target{nullptr};
signal_handler_t *signals_handler = nullptr;

/* Rates come derived from the dial loop (one computation shared by every
 * surface); "…" until a decode stream has two samples. */
QString rates_text(const lenslink_source_info &h)
{
	if (!h.has_rates)
		return QStringLiteral("…");

	QString text = QStringLiteral("%1 fps · %2 Mb/s")
			       .arg(qRound(h.fps))
			       .arg(h.bitrate_bps / 1e6, 0, 'f', 1);
	if (h.latency_ms > 0)
		text += QStringLiteral(" · %1 ms").arg(h.latency_ms);
	return text;
}

/* Which LensLink sources are visible through Studio Mode's Preview scene.
 * Empty (and `studio_mode` false) outside Studio Mode — the canvas of
 * plain single-view OBS has no Preview. A scene that can't be walked
 * safely leaves the set empty: no PVW beats a false PVW. */
void update_preview()
{
	preview_set.count = 0;
	studio_mode = false;
	if (!api.preview_program_mode_active ||
	    !api.get_current_preview_scene || !api.preview_program_mode_active())
		return;

	studio_mode = true;
	obs_source_t *scene = api.get_current_preview_scene();
	if (!scene)
		return;
	lenslink_preview_collect(scene, &preview_set);
	obs_source_release(scene);
}

const char *module_text(const char *key)
{
	return obs_module_text(key);
}

/* Rebuilds the model from the last snapshot and applies it to the dock.
 * Cheap (no allocation beyond the widgets' own text), so a click or a
 * reorder can call it directly. */
void render()
{
	if (!dock || !view)
		return;
	lenslink_rig_build(&rig, snapshot.data(), snapshot_count,
			   studio_mode ? &preview_set : nullptr, view.get());
	dock->apply(*view);
}

void refresh()
{
	if (!status_label || collection_busy)
		return;

	if (snapshot.empty())
		snapshot.resize(LENSLINK_MAX_SOURCES);
	snapshot_count = lenslink_source_enum(snapshot.data(), snapshot.size());
	const lenslink_source_info *list = snapshot.data();
	size_t count = snapshot_count;

	update_preview();
	render();

	QStringList bar_parts;
	int standby_count = 0;

	for (size_t i = 0; i < count; i++) {
		const lenslink_source_info &h = list[i];

		QString who = QString::fromUtf8(
			h.device_name[0] ? h.device_name : h.source_name);
		if (h.connected && !h.standby) {
			bar_parts << QStringLiteral("%1 %2").arg(
				who, rates_text(h));
		} else if (h.connected && h.standby) {
			standby_count++;
		}
	}

	/* Status bar: live streams get numbers; standby-only gets a word;
	 * nothing LensLink → no label at all (don't crowd OBS's bar). */
	QString prefix = QString::fromUtf8(obs_module_text("StatusBar.Label"));
	if (!bar_parts.isEmpty()) {
		status_label->setText(QStringLiteral("%1 %2").arg(
			prefix, bar_parts.join(QStringLiteral("  |  "))));
		status_label->setVisible(true);
	} else if (standby_count > 0) {
		status_label->setText(QStringLiteral("%1 %2").arg(
			prefix, QString::fromUtf8(
					obs_module_text("StatusBar.Standby"))));
		status_label->setVisible(true);
	} else {
		status_label->setVisible(false);
	}
}

/* Asks for a refresh from any thread. Many requests in one burst (a
 * transition activates and deactivates several sources) become one call
 * on the UI thread, which by then sees the settled state. */
void request_refresh()
{
	QObject *target = post_target.load();
	if (!target || refresh_pending.exchange(true))
		return;
	QMetaObject::invokeMethod(
		target,
		[] {
			refresh_pending.store(false);
			refresh();
		},
		Qt::QueuedConnection);
}

/* libobs signal callback — any thread. Only filters and posts. */
void source_signal(void *, calldata_t *cd)
{
	obs_source_t *source = nullptr;
	calldata_get_ptr(cd, "source", &source);
	if (source && lenslink_is_lenslink_source(source))
		request_refresh();
}

void connect_signals()
{
	signals_handler = obs_get_signal_handler();
	if (!signals_handler)
		return;
	/* Program: a source becoming or ceasing to be part of the final
	 * mix, through any nesting, and both sides of a transition. Plus
	 * the lifecycle events the 1 s timer would otherwise be the first
	 * to notice. */
	static const char *const names[] = {"source_activate",
					    "source_deactivate",
					    "source_create", "source_destroy",
					    "source_rename"};
	for (const char *name : names)
		signal_handler_connect(signals_handler, name, source_signal,
				       nullptr);
}

void disconnect_signals()
{
	post_target.store(nullptr);
	if (!signals_handler)
		return;
	static const char *const names[] = {"source_activate",
					    "source_deactivate",
					    "source_create", "source_destroy",
					    "source_rename"};
	/* Blocks until any callback already running has returned. */
	for (const char *name : names)
		signal_handler_disconnect(signals_handler, name, source_signal,
					  nullptr);
	signals_handler = nullptr;
}

/* --- persistence: the camera order, per scene collection ---------------- */

/* Save/load callback. Runs on the UI thread with the scene collection's
 * "modules" object. Loading replaces the order with the collection's own
 * (or none); saving writes only cameras that exist right now, in slot
 * order, keyed by OBS source UUID. */
void save_callback(obs_data_t *data, bool saving, void *)
{
	if (saving) {
		if (collection_busy)
			return; /* mid-switch: the registry is half a collection */

		if (snapshot.empty())
			snapshot.resize(LENSLINK_MAX_SOURCES);
		size_t n = lenslink_source_enum(snapshot.data(),
						snapshot.size());
		const char *present[LENSLINK_MAX_SOURCES];
		size_t np = 0;
		for (size_t i = 0; i < n; i++)
			if (snapshot[i].uuid[0])
				present[np++] = snapshot[i].uuid;

		char ordered[LENSLINK_RIG_MAX][LENSLINK_UUID_SIZE];
		size_t count = lenslink_rig_order_export(&rig.order, present,
							 np, ordered,
							 LENSLINK_RIG_MAX);

		obs_data_t *root = obs_data_create();
		obs_data_t *dock_data = obs_data_create();
		obs_data_array_t *order = obs_data_array_create();
		for (size_t i = 0; i < count; i++) {
			obs_data_t *item = obs_data_create();
			obs_data_set_string(item, "uuid", ordered[i]);
			obs_data_array_push_back(order, item);
			obs_data_release(item);
		}
		obs_data_set_array(dock_data, SAVE_ORDER, order);
		obs_data_set_obj(root, SAVE_DOCK, dock_data);
		obs_data_set_int(root, "version", SAVE_VERSION);
		obs_data_set_obj(data, SAVE_KEY, root);
		obs_data_array_release(order);
		obs_data_release(dock_data);
		obs_data_release(root);
		return;
	}

	lenslink_rig_order_clear(&rig.order);
	rig.selected[0] = '\0';
	rig.selected_slot = 0;

	obs_data_t *root = obs_data_get_obj(data, SAVE_KEY);
	if (!root)
		return;
	if (obs_data_get_int(root, "version") == SAVE_VERSION) {
		obs_data_t *dock_data = obs_data_get_obj(root, SAVE_DOCK);
		obs_data_array_t *order =
			dock_data ? obs_data_get_array(dock_data, SAVE_ORDER)
				  : nullptr;
		if (order) {
			char saved[LENSLINK_RIG_MAX][LENSLINK_UUID_SIZE];
			const char *ptrs[LENSLINK_RIG_MAX];
			size_t n = 0;
			size_t total = obs_data_array_count(order);
			for (size_t i = 0; i < total && n < LENSLINK_RIG_MAX;
			     i++) {
				obs_data_t *item = obs_data_array_item(order, i);
				const char *uuid =
					obs_data_get_string(item, "uuid");
				if (uuid && uuid[0]) {
					snprintf(saved[n], sizeof(saved[n]),
						 "%s", uuid);
					ptrs[n] = saved[n];
					n++;
				}
				obs_data_release(item);
			}
			lenslink_rig_order_import(&rig.order, ptrs, n);
			obs_data_array_release(order);
		}
		if (dock_data)
			obs_data_release(dock_data);
	}
	obs_data_release(root);
}

/* --- dock actions -------------------------------------------------------- */

void dock_select(const QString &uuid)
{
	if (!view)
		return;
	/* Dock-local state only; no OBS scene, preview or transition is
	 * touched by choosing a camera. */
	if (lenslink_rig_select(&rig, view.get(), uuid.toUtf8().constData()))
		render();
}

void dock_move(const QString &uuid, int delta)
{
	if (!view)
		return;
	const QByteArray key = uuid.toUtf8();
	bool screen = false;
	const char *keys[LENSLINK_MAX_SOURCES];
	size_t n = 0;
	for (size_t i = 0; i < view->count; i++)
		if (strcmp(view->cards[i].uuid, key.constData()) == 0)
			screen = view->cards[i].is_screen;
	/* Neighbours are the cards of the same kind, in slot order. */
	for (size_t i = 0; i < view->count; i++)
		if (view->cards[i].is_screen == screen)
			keys[n++] = view->cards[i].uuid;
	if (lenslink_rig_order_move(&rig.order, keys, n, key.constData(),
				    delta))
		render();
}

void dock_action(uint32_t id, lenslink::CardAction action)
{
	switch (action) {
	case lenslink::CardAction::Start:
		lenslink_source_stream_command(id, LENSLINK_STREAM_START);
		break;
	case lenslink::CardAction::Pause:
		lenslink_source_stream_command(id, LENSLINK_STREAM_PAUSE);
		break;
	case lenslink::CardAction::Resume:
		lenslink_source_stream_command(id, LENSLINK_STREAM_RESUME);
		break;
	case lenslink::CardAction::Stop:
		lenslink_source_stream_command(id, LENSLINK_STREAM_STOP);
		break;
	case lenslink::CardAction::Recalibrate:
		lenslink_source_recalibrate(id);
		break;
	}
}

void dock_open_properties(uint32_t id)
{
	if (!api.open_source_properties)
		return;
	/* A strong reference for the duration of the call, by id: the
	 * source may have been destroyed since the card was drawn. */
	if (obs_source_t *source = lenslink_source_get_obs_source(id)) {
		api.open_source_properties(source);
		obs_source_release(source);
	}
}

void create_widgets()
{
	auto *window =
		static_cast<QMainWindow *>(api.get_main_window());
	if (!window || status_label)
		return;

	status_label = new QLabel(window);
	status_label->setVisible(false);
	window->statusBar()->addPermanentWidget(status_label);

	view.reset(new lenslink_rig_view());

	if (api.add_dock_by_id) {
		lenslink::DockCallbacks callbacks;
		callbacks.select = dock_select;
		callbacks.move = dock_move;
		callbacks.open_properties = dock_open_properties;
		callbacks.action = dock_action;
		dock = new lenslink::RigDock(callbacks);
		if (!api.add_dock_by_id(DOCK_ID, obs_module_text("HealthDock"),
					dock)) {
			delete dock;
			dock = nullptr;
		}
	}

	timer = new QTimer(window);
	QObject::connect(timer, &QTimer::timeout, [] { refresh(); });
	timer->start(1000);
	post_target.store(timer);
	connect_signals();
	refresh();

	blog(LOG_INFO, "[lenslink] frontend UI ready (status bar%s)",
	     dock ? " + dock" : "");
}

void destroy_widgets()
{
	/* Signals first: after this returns no libobs thread can post to
	 * the timer or reach a widget. */
	disconnect_signals();
	if (timer) {
		timer->stop();
		timer->deleteLater();
		timer = nullptr;
	}
	if (status_label) {
		auto *window = static_cast<QMainWindow *>(
			api.get_main_window());
		if (window)
			window->statusBar()->removeWidget(status_label);
		status_label->deleteLater();
		status_label = nullptr;
	}
	if (dock) {
		/* OBS owns the widget; removing the dock destroys it. */
		if (api.remove_dock)
			api.remove_dock(DOCK_ID);
		dock = nullptr;
	}
	view.reset();
}

/* Tools -> "LensLink Settings": the plugin-wide switches that never made
 * sense per source. Modal, plain widgets, no moc. */
void show_settings_dialog(void *)
{
	QMainWindow *window =
		static_cast<QMainWindow *>(api.get_main_window());
	QDialog dialog(window);
	dialog.setWindowTitle(obs_module_text("Settings.Title"));

	auto *layout = new QVBoxLayout(&dialog);

	auto *gpu = new QCheckBox(obs_module_text("Settings.GpuPipeline"),
				  &dialog);
	auto *gpu_note =
		new QLabel(obs_module_text("Settings.GpuPipeline.Note"),
			   &dialog);
	gpu_note->setWordWrap(true);
	gpu_note->setStyleSheet(QStringLiteral("color: gray;"));

	auto *web = new QCheckBox(obs_module_text("Settings.Web"), &dialog);
	auto *port = new QSpinBox(&dialog);
	port->setRange(1024, 65535);
	auto *diag = new QCheckBox(obs_module_text("Settings.Diagnostics"),
				   &dialog);
	auto *dump = new QCheckBox(obs_module_text("Settings.Dump"), &dialog);
	auto *bench = new QCheckBox(obs_module_text("Settings.Benchmark"),
				    &dialog);
	bench->setToolTip(obs_module_text("Settings.Benchmark.Tip"));

	obs_data_t *current = lenslink_settings_snapshot();
	gpu->setChecked(obs_data_get_bool(current, LLS_GPU_PIPELINE));
	web->setChecked(obs_data_get_bool(current, LLS_WEB_ENABLED));
	port->setValue((int)obs_data_get_int(current, LLS_WEB_PORT));
	diag->setChecked(obs_data_get_bool(current, LLS_DIAGNOSTICS));
	dump->setChecked(obs_data_get_bool(current, LLS_DUMP_STREAM));
	bench->setChecked(obs_data_get_bool(current, LLS_BENCHMARK));
	obs_data_release(current);

	layout->addWidget(gpu);
	layout->addWidget(gpu_note);
	layout->addSpacing(12);
	layout->addWidget(web);
	auto *form = new QFormLayout();
	form->addRow(obs_module_text("Settings.WebPort"), port);
	layout->addLayout(form);
	layout->addSpacing(12);
	layout->addWidget(diag);
	layout->addWidget(dump);
	layout->addWidget(bench);

	/* Version footer, mirroring the source-properties one. */
	auto *version = new QLabel(
		QStringLiteral("LensLink " LENSLINK_VERSION), &dialog);
	version->setStyleSheet(
		QStringLiteral("color: gray; font-size: 11px;"));
	layout->addSpacing(8);
	layout->addWidget(version);

	/* Diagnostics is an action, not a setting, so it applies whether or
	 * not the dialog is accepted: the report describes the state the
	 * reporter is in right now. */
	auto *diagbtn = new QPushButton(
		obs_module_text("Settings.CopyDiagnostics"), &dialog);
	auto *diagnote = new QLabel(&dialog);
	diagnote->setWordWrap(true);
	diagnote->setStyleSheet(QStringLiteral("color: gray;"));
	QObject::connect(diagbtn, &QPushButton::clicked, [diagnote]() {
		char *report = lenslink_diagnostics_report();
		QGuiApplication::clipboard()->setText(
			QString::fromUtf8(report));
		bfree(report);
		lenslink_diagnostics_log();
		diagnote->setText(
			obs_module_text("Settings.CopyDiagnostics.Done"));
	});
	layout->addSpacing(12);
	layout->addWidget(diagbtn);
	layout->addWidget(diagnote);

	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok |
						     QDialogButtonBox::Cancel,
					     &dialog);
	QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog,
			 &QDialog::accept);
	QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog,
			 &QDialog::reject);
	layout->addWidget(buttons);

	if (dialog.exec() != QDialog::Accepted)
		return;

	obs_data_t *values = obs_data_create();
	obs_data_set_bool(values, LLS_GPU_PIPELINE, gpu->isChecked());
	obs_data_set_bool(values, LLS_WEB_ENABLED, web->isChecked());
	obs_data_set_int(values, LLS_WEB_PORT, port->value());
	obs_data_set_bool(values, LLS_DIAGNOSTICS, diag->isChecked());
	obs_data_set_bool(values, LLS_DUMP_STREAM, dump->isChecked());
	obs_data_set_bool(values, LLS_BENCHMARK, bench->isChecked());
	lenslink_settings_update(values);
	obs_data_release(values);

	/* Web/diagnostics apply immediately; the pipeline switch waits for
	 * the restart the note promised. */
	ios_camera_apply_global_settings();
}

void frontend_event(enum obs_frontend_event event, void *)
{
	switch (event) {
	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
		create_widgets();
		break;
	case OBS_FRONTEND_EVENT_EXIT:
		destroy_widgets();
		break;

	/* A new scene collection is a new rig: forget the old one's order,
	 * selection and Preview before the sources it names are destroyed,
	 * and don't query anything until the new collection is loaded. The
	 * new collection's own order arrives through the load callback,
	 * which runs between CLEANUP and CHANGED. */
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGING:
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CLEANUP:
		collection_busy = true;
		lenslink_rig_reset(&rig);
		preview_set.count = 0;
		studio_mode = false;
		snapshot_count = 0;
		if (dock)
			dock->clear();
		break;
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED:
		collection_busy = false;
		request_refresh();
		break;

	/* Tally: what is on air / in Preview changed. Requested rather than
	 * run inline so OBS has finished updating its own state first. */
	case OBS_FRONTEND_EVENT_SCENE_CHANGED:
	case OBS_FRONTEND_EVENT_PREVIEW_SCENE_CHANGED:
	case OBS_FRONTEND_EVENT_STUDIO_MODE_ENABLED:
	case OBS_FRONTEND_EVENT_STUDIO_MODE_DISABLED:
	case OBS_FRONTEND_EVENT_TRANSITION_CHANGED:
	case OBS_FRONTEND_EVENT_TRANSITION_STOPPED:
	case OBS_FRONTEND_EVENT_SCENE_LIST_CHANGED:
		request_refresh();
		break;

	case OBS_FRONTEND_EVENT_THEME_CHANGED:
		if (dock)
			dock->restyle();
		break;
	default:
		break;
	}
}

} // namespace

void lenslink_frontend_init(void)
{
	resolve(api.add_event_callback, "obs_frontend_add_event_callback");
	resolve(api.remove_event_callback,
		"obs_frontend_remove_event_callback");
	resolve(api.get_main_window, "obs_frontend_get_main_window");
	resolve(api.add_dock_by_id, "obs_frontend_add_dock_by_id");
	resolve(api.remove_dock, "obs_frontend_remove_dock");
	resolve(api.add_tools_menu_item, "obs_frontend_add_tools_menu_item");
	resolve(api.preview_program_mode_active,
		"obs_frontend_preview_program_mode_active");
	resolve(api.get_current_preview_scene,
		"obs_frontend_get_current_preview_scene");
	resolve(api.add_save_callback, "obs_frontend_add_save_callback");
	resolve(api.remove_save_callback, "obs_frontend_remove_save_callback");
	resolve(api.open_source_properties,
		"obs_frontend_open_source_properties");

	if (!api.add_event_callback || !api.remove_event_callback ||
	    !api.get_main_window) {
		blog(LOG_INFO,
		     "[lenslink] no OBS frontend in this process — "
		     "status-bar/dock UI disabled");
		return;
	}

	api.add_event_callback(frontend_event, nullptr);
	callback_installed = true;

	/* The order must be registered before the first scene collection
	 * loads (modules load first), so it is installed here, not with the
	 * widgets. Without the API the order simply isn't persisted. */
	lenslink_rig_init(&rig, module_text);
	if (api.add_save_callback && api.remove_save_callback) {
		api.add_save_callback(save_callback, nullptr);
		save_callback_installed = true;
	}

	if (api.add_tools_menu_item)
		api.add_tools_menu_item(obs_module_text("Settings.MenuItem"),
					show_settings_dialog, nullptr);
}

void lenslink_frontend_shutdown(void)
{
	if (callback_installed) {
		api.remove_event_callback(frontend_event, nullptr);
		callback_installed = false;
	}
	if (save_callback_installed) {
		api.remove_save_callback(save_callback, nullptr);
		save_callback_installed = false;
	}
	/* Widgets are torn down on OBS_FRONTEND_EVENT_EXIT, which precedes
	 * module unload; nothing further to do here. */
}
