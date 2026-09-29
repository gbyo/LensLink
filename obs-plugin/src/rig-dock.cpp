#include "rig-dock.h"

#include <obs-module.h>

#include <QAbstractButton>
#include <QAction>
#include <QLabel>
#include <QPushButton>
#include <QContextMenuEvent>
#include <QFontMetrics>
#include <QKeyEvent>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QBoxLayout>
#include <QPointer>

namespace lenslink {

namespace {

/* Mixes `b` into `a` by `t` (0 = a, 1 = b). */
QColor blend(const QColor &a, const QColor &b, qreal t)
{
	return QColor::fromRgbF(a.redF() * (1 - t) + b.redF() * t,
				a.greenF() * (1 - t) + b.greenF() * t,
				a.blueF() * (1 - t) + b.blueF() * t);
}

bool is_dark(const QWidget *w)
{
	return w->palette().color(QPalette::Window).lightness() < 128;
}

/* Warning amber, tuned per theme so text in it stays readable on both. */
QColor amber(const QWidget *w)
{
	return is_dark(w) ? QColor(0xF2, 0xB2, 0x33) : QColor(0xA8, 0x62, 0x00);
}

/* Tally colours are semantic (Program red, Preview green) and stay put
 * across themes; the badge text on them is white either way. */
const QColor kProgramRed(0xC6, 0x28, 0x28);
const QColor kPreviewGreen(0x2E, 0x7D, 0x32);

QString T_(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

/* Digits that don't change width as they change value, where the font
 * and Qt (6.7+) can do it: fps / Mb/s / ms don't make a card jitter. */
QFont tabular(QFont f)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
	f.setFeature("tnum", 1);
#endif
	return f;
}

} // namespace

/* A QLabel that paints itself from the live palette (so it follows any
 * OBS theme without a stylesheet), can elide, and can be tinted. */
class Label : public QLabel {
public:
	enum Tone { Primary, Secondary, Amber };

	Label(Tone tone, bool bold, bool elide, QWidget *parent = nullptr)
		: QLabel(parent), tone_(tone), elide_(elide)
	{
		QFont f = font();
		f.setBold(bold);
		setFont(f);
		if (elide)
			setSizePolicy(QSizePolicy::Ignored,
				      QSizePolicy::Preferred);
	}

	void setTone(Tone t)
	{
		if (t == tone_)
			return;
		tone_ = t;
		update();
	}

	QSize minimumSizeHint() const override
	{
		if (elide_)
			return QSize(fontMetrics().horizontalAdvance(
					     QStringLiteral("MMM")),
				     QLabel::minimumSizeHint().height());
		return QLabel::minimumSizeHint();
	}

protected:
	void paintEvent(QPaintEvent *) override
	{
		QPainter p(this);
		QColor c = palette().color(QPalette::WindowText);
		if (tone_ == Secondary)
			c = blend(c, palette().color(QPalette::Window), 0.42);
		else if (tone_ == Amber)
			c = amber(this);
		p.setPen(c);
		p.setFont(font());

		QRect r = contentsRect();
		QString shown = text();
		int flags = int(alignment());
		if (wordWrap())
			flags |= Qt::TextWordWrap;
		if (elide_)
			shown = fontMetrics().elidedText(shown, Qt::ElideRight,
							 r.width());
		p.drawText(r, flags, shown);
	}

private:
	Tone tone_;
	bool elide_;
};

namespace {

/* PGM / PVW: text as well as colour, on a compact pill. */
class Badge : public QWidget {
public:
	Badge(const QString &text, const QColor &color, bool bold,
	      QWidget *parent = nullptr)
		: QWidget(parent), text_(text), color_(color)
	{
		QFont f = font();
		f.setBold(bold);
		setFont(f);
		setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
		setVisible(false);
	}

	QSize sizeHint() const override
	{
		QFontMetrics fm(font());
		return QSize(fm.horizontalAdvance(text_) + 12,
			     fm.height() + 2);
	}

protected:
	void paintEvent(QPaintEvent *) override
	{
		QPainter p(this);
		p.setRenderHint(QPainter::Antialiasing);
		p.setPen(Qt::NoPen);
		p.setBrush(color_);
		p.drawRoundedRect(rect(), 4, 4);
		p.setPen(Qt::white);
		p.setFont(font());
		p.drawText(rect(), Qt::AlignCenter, text_);
	}

private:
	QString text_;
	QColor color_;
};

} // namespace

class CameraCard : public QWidget {
public:
	CameraCard(const DockCallbacks *callbacks, QWidget *parent)
		: QWidget(parent), cb_(callbacks)
	{
		setFocusPolicy(Qt::StrongFocus);
		setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);

		number_ = new Label(Label::Secondary, true, false);
		number_->setFont(tabular(number_->font()));
		name_ = new Label(Label::Primary, true, true);
		pgm_ = new Badge(T_("Dock.Program"), kProgramRed, true);
		pvw_ = new Badge(T_("Dock.Preview"), kPreviewGreen, false);
		pgm_->setAccessibleName(T_("Dock.OnAir"));
		pvw_->setAccessibleName(T_("Dock.InPreview"));

		glyph_ = new Label(Label::Amber, true, false);
		glyph_->setText(QStringLiteral("⚠"));
		glyph_->setVisible(false);
		health_ = new Label(Label::Secondary, false, false);
		health_->setWordWrap(true);
		health_->setAlignment(Qt::AlignLeft | Qt::AlignTop);
		health_->setFont(tabular(health_->font()));
		battery_ = new Label(Label::Secondary, false, false);
		battery_->setFont(tabular(battery_->font()));
		battery_->setAlignment(Qt::AlignRight | Qt::AlignTop);
		warning_ = new Label(Label::Amber, false, false);
		warning_->setWordWrap(true);
		warning_->setAlignment(Qt::AlignLeft | Qt::AlignTop);
		warning_->setVisible(false);
		note_ = new Label(Label::Secondary, false, false);
		note_->setWordWrap(true);
		note_->setAlignment(Qt::AlignLeft | Qt::AlignTop);
		note_->setVisible(false);

		auto *top = new QHBoxLayout;
		top->setSpacing(8);
		top->addWidget(number_, 0, Qt::AlignTop);
		top->addWidget(name_, 1);
		top->addWidget(pgm_, 0, Qt::AlignVCenter);
		top->addWidget(pvw_, 0, Qt::AlignVCenter);

		auto *mid = new QHBoxLayout;
		mid->setSpacing(6);
		mid->addWidget(glyph_, 0, Qt::AlignTop);
		mid->addWidget(health_, 1);
		mid->addWidget(battery_, 0, Qt::AlignTop);

		auto *root = new QVBoxLayout(this);
		root->setContentsMargins(14, 7, 10, 8);
		root->setSpacing(3);
		root->addLayout(top);
		root->addLayout(mid);
		root->addWidget(warning_);
		root->addWidget(note_);
	}

	void apply(const lenslink_card &c, bool can_up, bool can_down)
	{
		uuid_ = QString::fromUtf8(c.uuid);
		id_ = c.id;
		card_ = c;
		can_up_ = can_up;
		can_down_ = can_down;

		bool tally_changed = c.program != program_ ||
				     c.preview != preview_ ||
				     c.selected != selected_;
		program_ = c.program;
		preview_ = c.preview;
		selected_ = c.selected;

		setCursor(c.is_screen ? Qt::ArrowCursor : Qt::PointingHandCursor);
		number_->setVisible(!c.is_screen);
		set(number_, QString::number(c.number));
		set(name_, QString::fromUtf8(c.name));
		pgm_->setVisible(c.program);
		pvw_->setVisible(c.preview);

		bool flagged = c.attention >= ATTENTION_WARNING;
		glyph_->setVisible(flagged);
		set(health_, QString::fromUtf8(c.health));
		health_->setVisible(c.health[0] != '\0');
		set(battery_, QString::fromUtf8(c.battery));
		battery_->setVisible(c.battery[0] != '\0');
		battery_->setTone(c.battery_low ? Label::Amber
						: Label::Secondary);

		set(warning_, QString::fromUtf8(c.warning));
		warning_->setVisible(c.warning[0] != '\0');
		warning_->setTone(Label::Amber);
		set(note_, QString::fromUtf8(c.note));
		note_->setVisible(c.note[0] != '\0');

		QString tip = QString::fromUtf8(c.tooltip);
		if (toolTip() != tip)
			setToolTip(tip);
		QString a11y = QString::fromUtf8(c.accessible);
		if (accessibleName() != a11y)
			setAccessibleName(a11y);

		if (tally_changed)
			update();
	}

	const QString &uuid() const { return uuid_; }

protected:
	void paintEvent(QPaintEvent *) override
	{
		QPainter p(this);
		p.setRenderHint(QPainter::Antialiasing);
		const QColor window = palette().color(QPalette::Window);
		const QColor text = palette().color(QPalette::WindowText);
		QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);

		QPainterPath shape;
		shape.addRoundedRect(r, 6, 6);
		/* A card reads as a card on any theme: a touch lighter than a
		 * dark window, a touch lighter than a light one. */
		p.fillPath(shape, is_dark(this) ? blend(window, text, 0.07)
						: blend(window, Qt::white, 0.6));

		/* A narrow tally bar, not a repainted card: red for Program,
		 * green for Preview, both when it is both. */
		if (program_ || preview_) {
			p.save();
			p.setClipPath(shape);
			QRectF bar(r.left(), r.top(), 4, r.height());
			if (program_ && preview_) {
				p.fillRect(bar.adjusted(0, 0, 0, -r.height() / 2),
					   kProgramRed);
				p.fillRect(bar.adjusted(0, r.height() / 2, 0, 0),
					   kPreviewGreen);
			} else {
				p.fillRect(bar, program_ ? kProgramRed
							 : kPreviewGreen);
			}
			p.restore();
		}

		/* Selection: the accent, thicker — never red or green, which
		 * belong to tally. */
		p.setBrush(Qt::NoBrush);
		if (selected_) {
			p.setPen(QPen(palette().color(QPalette::Highlight), 2));
			p.drawRoundedRect(r.adjusted(1, 1, -1, -1), 5, 5);
		} else {
			p.setPen(QPen(blend(window, text, 0.22), 1));
			p.drawRoundedRect(r, 6, 6);
		}
		if (hasFocus()) {
			QPen ring(palette().color(QPalette::Highlight), 1,
				  Qt::DotLine);
			p.setPen(ring);
			p.drawRoundedRect(r.adjusted(3, 3, -3, -3), 4, 4);
		}
	}

	void mousePressEvent(QMouseEvent *e) override
	{
		if (e->button() == Qt::LeftButton && !card_.is_screen &&
		    cb_->select)
			cb_->select(uuid_);
		QWidget::mousePressEvent(e);
	}

	void keyPressEvent(QKeyEvent *e) override
	{
		switch (e->key()) {
		case Qt::Key_Return:
		case Qt::Key_Enter:
		case Qt::Key_Space:
			if (!card_.is_screen && cb_->select)
				cb_->select(uuid_);
			return;
		case Qt::Key_Down:
			focusNextChild();
			return;
		case Qt::Key_Up:
			focusPreviousChild();
			return;
		default:
			QWidget::keyPressEvent(e);
		}
	}

	void focusInEvent(QFocusEvent *e) override
	{
		QWidget::focusInEvent(e);
		update();
	}
	void focusOutEvent(QFocusEvent *e) override
	{
		QWidget::focusOutEvent(e);
		update();
	}
	void changeEvent(QEvent *e) override
	{
		QWidget::changeEvent(e);
		if (e->type() == QEvent::PaletteChange ||
		    e->type() == QEvent::StyleChange)
			update();
	}

	void contextMenuEvent(QContextMenuEvent *e) override
	{
		/* A child popup: if this card is deleted meanwhile (its source
		 * removed), the menu goes with it — nothing dangles. Actions
		 * copy what they need. */
		auto *menu = new QMenu(this);
		menu->setAttribute(Qt::WA_DeleteOnClose);

		const DockCallbacks cb = *cb_;
		const uint32_t id = id_;
		const QString name = QString::fromUtf8(card_.name);
		const bool on_air = card_.program;
		QWidget *dialog_parent = window();

		auto act = [&](const QString &label, bool enabled, auto fn) {
			QAction *a = menu->addAction(label);
			a->setEnabled(enabled);
			QObject::connect(a, &QAction::triggered, menu, fn);
		};
		auto simple = [cb, id](CardAction what) {
			return [cb, id, what] {
				if (cb.action)
					cb.action(id, what);
			};
		};
		/* Disruptive on air: say so first, then act. */
		auto confirmed = [cb, id, name, on_air,
				  dialog_parent](CardAction what,
						 const QString &title,
						 const QString &body,
						 const QString &button) {
			return [=] {
				if (!on_air) {
					if (cb.action)
						cb.action(id, what);
					return;
				}
				auto *box = new QMessageBox(
					QMessageBox::Warning, title, QString(),
					QMessageBox::NoButton, dialog_parent);
				box->setAttribute(Qt::WA_DeleteOnClose);
				box->setTextFormat(Qt::PlainText);
				box->setText(title);
				box->setInformativeText(name + QStringLiteral("\n\n") +
							body);
				QPushButton *cancel = box->addButton(
					T_("Dock.Confirm.Cancel"),
					QMessageBox::RejectRole);
				QPushButton *go = box->addButton(
					button, QMessageBox::DestructiveRole);
				box->setDefaultButton(cancel);
				QObject::connect(go, &QAbstractButton::clicked,
						 box, [cb, id, what] {
							 if (cb.action)
								 cb.action(id,
									   what);
						 });
				box->setWindowModality(Qt::WindowModal);
				box->open();
			};
		};

		if (card_.can_start)
			act(T_("Dock.Action.Start"), true,
			    simple(CardAction::Start));
		if (card_.can_pause)
			act(T_("Dock.Action.Pause"), true,
			    confirmed(CardAction::Pause,
				      T_("Dock.Confirm.Pause.Title"),
				      T_("Dock.Confirm.Pause.Text"),
				      T_("Dock.Action.Pause")));
		if (card_.can_resume)
			act(T_("Dock.Action.Resume"), true,
			    simple(CardAction::Resume));
		if (card_.can_recalibrate)
			act(T_("Dock.Action.Recalibrate"), true,
			    simple(CardAction::Recalibrate));
		act(T_("Dock.Action.Properties"), true, [cb, id] {
			if (cb.open_properties)
				cb.open_properties(id);
		});

		if (cb.move && (can_up_ || can_down_)) {
			menu->addSeparator();
			const QString uuid = uuid_;
			act(T_("Dock.Action.MoveUp"), can_up_,
			    [cb, uuid] { cb.move(uuid, -1); });
			act(T_("Dock.Action.MoveDown"), can_down_,
			    [cb, uuid] { cb.move(uuid, +1); });
		}

		/* The destructive one lives alone at the bottom. */
		if (card_.can_stop) {
			menu->addSeparator();
			act(T_("Dock.Action.Stop"), true,
			    confirmed(CardAction::Stop,
				      T_("Dock.Confirm.Stop.Title"),
				      T_("Dock.Confirm.Stop.Text"),
				      T_("Dock.Action.Stop")));
		}

		menu->popup(e->globalPos());
	}

private:
	static void set(Label *label, const QString &text)
	{
		if (label->text() != text)
			label->setText(text);
	}

	const DockCallbacks *cb_;
	QString uuid_;
	uint32_t id_ = 0;
	lenslink_card card_ = {};
	bool program_ = false, preview_ = false, selected_ = false;
	bool can_up_ = false, can_down_ = false;

	Label *number_, *name_, *glyph_, *health_, *battery_, *warning_, *note_;
	Badge *pgm_, *pvw_;
};

/* --- RigDock ------------------------------------------------------------ */

RigDock::RigDock(DockCallbacks callbacks, QWidget *parent)
	: QWidget(parent), callbacks_(std::move(callbacks))
{
	setAccessibleName(T_("HealthDock"));

	headline_ = new Label(Label::Primary, true, false);
	headline_->setWordWrap(true);
	headline_->setFont(tabular(headline_->font()));
	status_ = new Label(Label::Secondary, false, false);
	status_->setWordWrap(true);

	summary_ = new QWidget;
	auto *summary_layout = new QVBoxLayout(summary_);
	summary_layout->setContentsMargins(12, 10, 12, 6);
	summary_layout->setSpacing(1);
	summary_layout->addWidget(headline_);
	summary_layout->addWidget(status_);

	empty_ = new Label(Label::Secondary, false, false);
	empty_->setWordWrap(true);
	empty_->setText(T_("HealthDock.Empty"));

	screens_header_ = new Label(Label::Secondary, true, false);
	screens_header_->setText(T_("Dock.Screens"));
	screens_header_->setVisible(false);

	content_ = new QWidget;
	auto *list = new QVBoxLayout(content_);
	list->setContentsMargins(8, 4, 8, 8);
	list->setSpacing(6);
	list->addWidget(empty_);
	list->addWidget(screens_header_);
	list->addStretch(1);
	list_ = list;

	scroll_ = new QScrollArea;
	scroll_->setWidgetResizable(true);
	scroll_->setFrameShape(QFrame::NoFrame);
	scroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	scroll_->setWidget(content_);

	auto *root = new QVBoxLayout(this);
	root->setContentsMargins(0, 0, 0, 0);
	root->setSpacing(0);
	root->addWidget(summary_);
	root->addWidget(scroll_, 1);

	summary_->setVisible(false);
	setMinimumWidth(180);
}

void RigDock::clear()
{
	for (CameraCard *card : cards_)
		delete card;
	cards_.clear();
	screens_header_->setVisible(false);
	summary_->setVisible(false);
	empty_->setVisible(true);
}

void RigDock::restyle()
{
	update();
	for (CameraCard *card : cards_)
		card->update();
	for (QWidget *w : findChildren<QWidget *>())
		w->update();
}

void RigDock::apply(const lenslink_rig_view &view)
{
	auto *box = static_cast<QBoxLayout *>(list_);

	empty_->setVisible(view.count == 0);
	summary_->setVisible(view.count != 0);

	/* Drop cards whose source is gone. */
	QHash<QString, bool> wanted;
	for (size_t i = 0; i < view.count; i++)
		wanted.insert(QString::fromUtf8(view.cards[i].uuid), true);
	for (auto it = cards_.begin(); it != cards_.end();) {
		if (!wanted.contains(it.key())) {
			delete it.value();
			it = cards_.erase(it);
		} else {
			++it;
		}
	}

	/* Desired layout order: [empty] cameras…, screens header, screens…,
	 * stretch. `pos` walks it; a widget is moved only if it isn't
	 * already where it belongs. */
	int pos = 1; /* index 0 is the empty-state label */
	auto place = [&](QWidget *w) {
		if (box->itemAt(pos) && box->itemAt(pos)->widget() == w) {
			pos++;
			return;
		}
		box->removeWidget(w);
		box->insertWidget(pos++, w);
	};

	bool screens_started = false;
	size_t screens = view.count - view.camera_count;
	for (size_t i = 0; i < view.count; i++) {
		const lenslink_card &c = view.cards[i];
		if (c.is_screen && !screens_started) {
			screens_started = true;
			place(screens_header_);
		}
		const QString key = QString::fromUtf8(c.uuid);
		CameraCard *card = cards_.value(key, nullptr);
		if (!card) {
			card = new CameraCard(&callbacks_, content_);
			cards_.insert(key, card);
		}
		place(card);

		size_t first = c.is_screen ? view.camera_count : 0;
		size_t last = c.is_screen ? view.count : view.camera_count;
		card->apply(c, i > first, i + 1 < last);
	}
	screens_header_->setVisible(screens > 0);

	if (view.count) {
		const lenslink_rig_summary &s = view.summary;
		QString headline = QString::fromUtf8(s.headline);
		if (headline_->text() != headline)
			headline_->setText(headline);
		QString status = QString::fromUtf8(s.status);
		if (status_->text() != status)
			status_->setText(status);
		status_->setVisible(!status.isEmpty());
		status_->setTone(s.attention > 0 ? Label::Amber
						 : Label::Secondary);
		QString summary_a11y = headline + QStringLiteral(". ") + status;
		if (summary_->accessibleName() != summary_a11y)
			summary_->setAccessibleName(summary_a11y);
	}
}

} // namespace lenslink
