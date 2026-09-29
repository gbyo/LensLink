/*
 * The widgets of the LensLink dock: a compact, scannable strip of camera
 * cards under a one-glance rig summary. Pure presentation — everything it
 * shows arrives as a `lenslink_rig_view` (rig-model.h) and everything the
 * operator does leaves through `Callbacks`, so this file touches neither
 * the registry, the OBS frontend nor libobs.
 *
 *   ┌ summary: "4 cameras · 3 live · 38.7 Mb/s" / "All healthy" ┐
 *   ├ scroll area ───────────────────────────────────────────────┤
 *   │  ┌ card ─────────────────────────────┐                     │
 *   │  │ 1  Stage Left              PGM    │  number, name, tally│
 *   │  │ 60 fps · 11.2 Mb/s · 39 ms · USB  92% ⚡│  health, battery │
 *   │  │ Camera thermal pressure           │  only when needed   │
 *   │  └───────────────────────────────────┘                     │
 *   │  Screens … (secondary rows, no numbers, no selection)      │
 *   └────────────────────────────────────────────────────────────┘
 *
 * Cards are kept and updated in place (keyed by the OBS source UUID): a
 * numeric refresh changes label text, never the widget tree, so nothing
 * flickers, scrolls or loses its tooltip. Colours are read from the live
 * palette at paint time, so an OBS theme change needs only a repaint.
 * No Q_OBJECT anywhere: the plugin's plain CMake stays plain.
 */

#pragma once

#include <QHash>
#include <QScrollArea>
#include <QString>
#include <QWidget>

#include <functional>

#include "rig-model.h"

namespace lenslink {

enum class CardAction { Start, Pause, Resume, Stop, Recalibrate };

struct DockCallbacks {
	/* Primary click / Enter: choose the camera for LensLink control.
	 * By contract this changes nothing in OBS. */
	std::function<void(const QString &uuid)> select;
	/* Context menu: nudge a card one slot (-1 up, +1 down). */
	std::function<void(const QString &uuid, int delta)> move;
	std::function<void(uint32_t id)> open_properties;
	/* The card already confirmed disruptive actions on a Program source. */
	std::function<void(uint32_t id, CardAction action)> action;
};

class CameraCard;
class Label;

class RigDock : public QWidget {
public:
	explicit RigDock(DockCallbacks callbacks, QWidget *parent = nullptr);

	/* Reconciles the cards with `view`: adds, removes, reorders and
	 * updates only what changed. */
	void apply(const lenslink_rig_view &view);

	/* Drops every card (scene collection change). */
	void clear();

	/* The OBS theme changed: repaint everything from the new palette. */
	void restyle();

private:
	DockCallbacks callbacks_;
	Label *headline_;
	Label *status_;
	Label *empty_;
	Label *screens_header_;
	QWidget *summary_;
	QScrollArea *scroll_;
	QWidget *content_;
	QLayout *list_;
	QHash<QString, CameraCard *> cards_;
};

} // namespace lenslink
