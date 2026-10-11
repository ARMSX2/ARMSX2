// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#include "QtShot.h"

#include "GameList/GameListWidget.h"
#include "MainWindow.h"
#include "QtHost.h"
#include "Settings/SettingsWindow.h"

#include "pcsx2/GameList.h"

#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtCore/QEventLoop>
#include <QtCore/QFileInfo>
#include <QtCore/QSet>
#include <QtCore/QTimer>
#include <QtGui/QAction>
#include <QtGui/QGuiApplication>
#include <QtGui/QPixmap>
#include <QtWidgets/QAbstractButton>
#include <QtWidgets/QApplication>
#include <QtWidgets/QDialog>
#include <QtWidgets/QListWidget>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QStackedWidget>
#include <QtWidgets/QTabBar>
#include <QtWidgets/QTabWidget>

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <utility>
#include <vector>

namespace
{
	struct Shot
	{
		QString spec;
		QString path;
	};

	struct DialogEntry
	{
		QString name;
		QString description;
		QtShot::DialogFactory factory;
	};

	struct Request
	{
		std::vector<Shot> shots;
		QString game_path;
		QSize size;
		int wait_ms = 300;
		int timeout_s = 120;
		bool error = false;
	};

	Request s_request;
	QtShot::Context s_context;
	bool s_timed_out = false;

	std::vector<DialogEntry>& Dialogs()
	{
		static std::vector<DialogEntry> dialogs; // function-local: the registrars run during static initialisation
		return dialogs;
	}

	// Every line the driver script reads starts with "QTSHOT ".
	void Say(const QString& line)
	{
		std::fprintf(stdout, "QTSHOT %s\n", line.toUtf8().constData());
		std::fflush(stdout);
	}

	// "Memory Cards", "memory-cards" and "MemoryCard" must all be the same page name.
	QString Key(const QString& s)
	{
		QString key;
		for (const QChar c : s)
		{
			if (c.isLetterOrNumber())
				key += c.toLower();
		}
		return key;
	}

	QString Slug(const QString& s)
	{
		QString slug;
		for (const QChar c : s)
		{
			if (c.isLetterOrNumber())
				slug += c.toLower();
			else if (!slug.isEmpty() && !slug.endsWith('-'))
				slug += '-';
		}
		while (slug.endsWith('-'))
			slug.chop(1);
		return slug;
	}

	// DEV9SettingsWidget -> dev9, MemoryCardSettingsWidget -> memorycard.
	QString ClassKey(const QWidget* page)
	{
		QString name = QString::fromLatin1(page->metaObject()->className());
		for (const QLatin1String suffix : {QLatin1String("SettingsWidget"), QLatin1String("Widget")})
		{
			if (name.endsWith(suffix) && name.size() > suffix.size())
			{
				name.chop(suffix.size());
				break;
			}
		}
		return Key(name);
	}

	[[noreturn]] void Refuse(const QString& why)
	{
		Say(QStringLiteral("fail %1").arg(why));
		std::_Exit(2); // before the event loop and before any window exists; nothing to unwind
	}

	void Pump(int ms)
	{
		QEventLoop loop;
		QTimer::singleShot(ms, &loop, &QEventLoop::quit);
		loop.exec();
	}

	// Grabs and dismisses a modal dialog the moment it has been up for the settle time. A button that opens a
	// dialog with exec() blocks inside click(), so this is what turns it into a picture. Polling rather than
	// a hook, so it needs nothing from the dialog.
	class ModalWatcher final : public QObject
	{
	public:
		ModalWatcher()
		{
			connect(&m_timer, &QTimer::timeout, this, &ModalWatcher::tick);
			m_timer.start(50);
		}

		void reset()
		{
			pixmap = QPixmap();
			title.clear();
		}

		void setSettle(int settle_ms) { m_settle_ms = settle_ms; }

		QPixmap pixmap;
		QString title;

	private:
		void tick()
		{
			QWidget* modal = QApplication::activeModalWidget();
			if (!modal)
			{
				m_seen = nullptr;
				return;
			}
			if (modal != m_seen)
			{
				m_seen = modal;
				m_since.restart();
				return;
			}
			if (m_since.elapsed() < m_settle_ms)
				return;

			pixmap = modal->grab();
			title = modal->windowTitle();
			m_seen = nullptr;
			if (QDialog* dialog = qobject_cast<QDialog*>(modal))
				dialog->reject();
			else
				modal->close();
		}

		QTimer m_timer;
		QElapsedTimer m_since;
		QWidget* m_seen = nullptr;
		int m_settle_ms = 300;
	};

	ModalWatcher* s_modal_watcher = nullptr;

	struct SettingsPage
	{
		int row;
		QString title;
		QString class_key;
		QWidget* widget;
	};

	// The category list and page stack of a SettingsWindow, by their names in SettingsWindow.ui.
	bool GetSettingsPages(SettingsWindow* window, QListWidget** list_out, std::vector<SettingsPage>* pages_out)
	{
		QListWidget* list = window->findChild<QListWidget*>(QStringLiteral("settingsCategory"));
		QStackedWidget* stack = window->findChild<QStackedWidget*>(QStringLiteral("settingsContainer"));
		if (!list || !stack)
			return false;

		for (int i = 0; i < list->count() && i < stack->count(); i++)
			pages_out->push_back({i, list->item(i)->text(), ClassKey(stack->widget(i)), stack->widget(i)});
		*list_out = list;
		return true;
	}

	SettingsWindow* FindSettingsWindow(bool per_game)
	{
		for (QWidget* widget : QApplication::topLevelWidgets())
		{
			SettingsWindow* window = qobject_cast<SettingsWindow*>(widget);
			if (window && window->isPerGameSettings() == per_game && window->isVisible())
				return window;
		}
		return nullptr;
	}

	QString PageNames(SettingsWindow* window)
	{
		QListWidget* list;
		std::vector<SettingsPage> pages;
		QStringList names;
		if (GetSettingsPages(window, &list, &pages))
		{
			for (const SettingsPage& page : pages)
				names << Slug(page.title);
		}
		return names.join(QStringLiteral(", "));
	}

	bool SelectPage(SettingsWindow* window, const QString& name, QString* error)
	{
		QListWidget* list;
		std::vector<SettingsPage> pages;
		if (!GetSettingsPages(window, &list, &pages))
		{
			*error = QStringLiteral("SettingsWindow has no settingsCategory/settingsContainer; was SettingsWindow.ui renamed?");
			return false;
		}

		const QString key = Key(name);
		for (const SettingsPage& page : pages)
		{
			if (key == Key(page.title) || key == page.class_key)
			{
				list->setCurrentRow(page.row);
				list->setFocus(); // where a freshly opened window has it; the reused global window may not
				return true;
			}
		}

		*error = QStringLiteral("no settings page '%1'; pages in this window: %2").arg(name, PageNames(window));
		return false;
	}

	// Shows whatever contains `widget`: the tab, the stacked page, the settings category, the scroll position.
	void Reveal(QWidget* widget)
	{
		QWidget* child = widget;
		for (QWidget* parent = widget->parentWidget(); parent; child = parent, parent = parent->parentWidget())
		{
			QStackedWidget* stack = qobject_cast<QStackedWidget*>(parent);
			if (!stack)
				continue;
			const int index = stack->indexOf(child);
			if (index < 0)
				continue;

			if (QTabWidget* tabs = qobject_cast<QTabWidget*>(stack->parentWidget()))
				tabs->setCurrentIndex(index);
			else if (QListWidget* list = stack->objectName() == QLatin1String("settingsContainer") ?
			                                 stack->parentWidget()->findChild<QListWidget*>(QStringLiteral("settingsCategory")) :
			                                 nullptr)
				list->setCurrentRow(index); // keeps the highlighted category in step with the page
			else
				stack->setCurrentIndex(index);
		}

		Pump(50); // let the newly shown pages lay out before asking a scroll area for geometry
		for (QWidget* parent = widget->parentWidget(); parent; parent = parent->parentWidget())
		{
			if (QScrollArea* scroll = qobject_cast<QScrollArea*>(parent))
				scroll->ensureWidgetVisible(widget);
		}
		widget->setFocus(Qt::OtherFocusReason);
		if (!widget->hasFocus())
			Say(QStringLiteral("warn focus=%1: the widget cannot take focus (hidden, disabled or not focusable)").arg(widget->objectName()));
	}

	bool SelectTab(QWidget* window, const QString& text, QString* error)
	{
		const QString key = Key(text);
		QStringList seen;
		for (QTabWidget* tabs : window->findChildren<QTabWidget*>())
		{
			if (!tabs->isVisible())
				continue;
			for (int i = 0; i < tabs->count(); i++)
			{
				seen << tabs->tabText(i).remove('&');
				if (Key(tabs->tabText(i)) == key)
				{
					tabs->setCurrentIndex(i);
					return true;
				}
			}
		}
		// A tab bar used on its own (a filter strip over a list), not the bar of a tab widget.
		for (QTabBar* bar : window->findChildren<QTabBar*>())
		{
			if (!bar->isVisible() || qobject_cast<QTabWidget*>(bar->parentWidget()))
				continue;
			for (int i = 0; i < bar->count(); i++)
			{
				seen << bar->tabText(i).remove('&');
				if (bar->isTabVisible(i) && Key(bar->tabText(i)) == key)
				{
					bar->setCurrentIndex(i);
					return true;
				}
			}
		}
		*error = seen.isEmpty() ? QStringLiteral("no visible tab widget in this window") :
		                          QStringLiteral("no visible tab '%1'; tabs here: %2").arg(text, seen.join(QStringLiteral(", ")));
		return false;
	}

	bool Click(QWidget* window, const QString& name, QString* error)
	{
		if (QAbstractButton* button = window->findChild<QAbstractButton*>(name))
		{
			if (!button->isEnabled())
			{
				*error = QStringLiteral("button '%1' is disabled").arg(name);
				return false;
			}
			button->click();
			return true;
		}

		QAction* action = window->findChild<QAction*>(name);
		if (!action && g_main_window)
			action = g_main_window->findChild<QAction*>(name);
		if (action)
		{
			if (!action->isEnabled())
			{
				*error = QStringLiteral("action '%1' is disabled").arg(name);
				return false;
			}
			action->trigger();
			return true;
		}

		*error = QStringLiteral("no button or action named '%1'; objectNames are the widget names in the .ui file").arg(name);
		return false;
	}

	QWidget* FindWidget(QWidget* window, const QString& name, QString* error)
	{
		QWidget* widget = window->findChild<QWidget*>(name);
		if (!widget)
			*error = QStringLiteral("no widget named '%1'; objectNames are the widget names in the .ui file").arg(name);
		return widget;
	}

	bool ParseSize(const QString& text, QSize* out)
	{
		const QStringList parts = text.toLower().split('x');
		bool ok_w = false, ok_h = false;
		const int w = parts.size() == 2 ? parts[0].toInt(&ok_w) : 0;
		const int h = parts.size() == 2 ? parts[1].toInt(&ok_h) : 0;
		if (!ok_w || !ok_h || w < 1 || h < 1)
			return false;
		*out = QSize(w, h);
		return true;
	}

	struct Opened
	{
		QWidget* window = nullptr;
		bool global_settings = false; // owned by MainWindow: kept, and put back as it was, instead of deleted
	};

	// The global settings window lives for the whole run, so a shot that resizes it or picks a tab would
	// show up in the next one. This puts those back.
	struct ViewState
	{
		QSize size;
		std::vector<std::pair<QTabWidget*, int>> tabs;

		explicit ViewState(QWidget* window)
			: size(window->size())
		{
			for (QTabWidget* tab : window->findChildren<QTabWidget*>())
				tabs.emplace_back(tab, tab->currentIndex());
		}

		void restore(QWidget* window) const
		{
			window->resize(size);
			for (const auto& [tab, index] : tabs)
				tab->setCurrentIndex(index);
		}
	};

	bool OpenTarget(const QString& target, Opened* opened, QString* error)
	{
		const int colon = target.indexOf(':');
		const QString kind = colon < 0 ? target : target.left(colon);
		const QString name = colon < 0 ? QString() : target.mid(colon + 1);

		if (kind == QLatin1String("main") && name.isEmpty())
		{
			opened->window = g_main_window;
			g_main_window->show();
			return true;
		}

		if (kind == QLatin1String("settings"))
		{
			g_main_window->doSettings();
			SettingsWindow* window = FindSettingsWindow(false);
			if (!window)
			{
				*error = QStringLiteral("the global settings window did not open");
				return false;
			}
			opened->window = window;
			opened->global_settings = true;
			return SelectPage(window, name, error);
		}

		if (kind == QLatin1String("game"))
		{
			if (s_context.game_path.isEmpty())
			{
				*error = QStringLiteral("game:* shots need -uishot-game <path>");
				return false;
			}

			std::optional<GameList::Entry> entry;
			{
				auto lock = GameList::GetLock();
				if (const GameList::Entry* found = GameList::GetEntryForPath(s_context.game_path.toUtf8().constData()))
					entry = *found;
			}
			if (!entry)
			{
				*error = QStringLiteral("'%1' is not in the game list; it must sit in a scanned game directory").arg(s_context.game_path);
				return false;
			}

			SettingsWindow::openGamePropertiesDialog(
				&*entry, entry->title, entry->serial, entry->crc, entry->type == GameList::EntryType::ELF, nullptr);
			SettingsWindow* window = FindSettingsWindow(true);
			if (!window)
			{
				*error = QStringLiteral("the per-game settings window did not open");
				return false;
			}
			opened->window = window;
			return SelectPage(window, name, error);
		}

		if (kind == QLatin1String("dialog"))
		{
			QStringList names;
			for (const DialogEntry& entry : Dialogs())
			{
				if (Key(entry.name) != Key(name))
				{
					names << entry.name;
					continue;
				}
				QWidget* widget = entry.factory(s_context);
				if (!widget)
				{
					*error = QStringLiteral("the factory for dialog '%1' returned nothing").arg(name);
					return false;
				}
				opened->window = widget;
				widget->show();
				return true;
			}
			*error = QStringLiteral("no dialog '%1'; registered: %2").arg(name, names.join(QStringLiteral(", ")));
			return false;
		}

		*error = QStringLiteral("unknown target '%1'; use main, settings:<page>, game:<page>, dialog:<name> or list").arg(target);
		return false;
	}

	void PrintList()
	{
		Say(QStringLiteral("list main"));

		g_main_window->doSettings();
		if (SettingsWindow* window = FindSettingsWindow(false))
		{
			QListWidget* list;
			std::vector<SettingsPage> pages;
			if (GetSettingsPages(window, &list, &pages))
			{
				for (const SettingsPage& page : pages)
					Say(QStringLiteral("list settings:%1 \"%2\" %3").arg(Slug(page.title), page.title, page.class_key));
			}
		}

		if (s_context.game_path.isEmpty())
		{
			Say(QStringLiteral("list game:<page> (give -uishot-game <path> to list the per-game pages)"));
		}
		else
		{
			QString error;
			Opened opened;
			if (OpenTarget(QStringLiteral("game:summary"), &opened, &error))
			{
				QListWidget* list;
				std::vector<SettingsPage> pages;
				if (GetSettingsPages(static_cast<SettingsWindow*>(opened.window), &list, &pages))
				{
					for (const SettingsPage& page : pages)
						Say(QStringLiteral("list game:%1 \"%2\" %3").arg(Slug(page.title), page.title, page.class_key));
				}
				opened.window->close();
			}
			else
			{
				Say(QStringLiteral("list game:<page> unavailable: %1").arg(error));
			}
		}

		for (const DialogEntry& entry : Dialogs())
			Say(QStringLiteral("list dialog:%1 %2").arg(entry.name, entry.description));
	}

	bool RunShot(const Shot& shot)
	{
		const QStringList parts = shot.spec.split('|');
		const QString target = parts[0].trimmed();
		QString error;

		auto fail = [&](const QString& why) {
			Say(QStringLiteral("fail %1: %2").arg(shot.spec, why));
			return false;
		};

		if (target == QLatin1String("list"))
		{
			PrintList();
			return true;
		}

		int settle_ms = s_request.wait_ms;
		s_modal_watcher->reset();
		s_modal_watcher->setSettle(settle_ms);

		QSet<QWidget*> before;
		for (QWidget* widget : QApplication::topLevelWidgets())
		{
			if (widget->isVisible())
				before.insert(widget);
		}

		// Leaves the UI as found, so the next shot starts clean. Deleted rather than closed: deleteLater would not
		// run until the shots end (they sit in a nested event loop), and a per-game window that is still alive
		// gets reused by the next shot.
		Opened opened;
		std::unique_ptr<ViewState> view;
		auto cleanup = [&]() {
			for (QWidget* widget : QApplication::topLevelWidgets())
			{
				if (widget->isVisible() && !before.contains(widget) && widget != opened.window)
					widget->close();
			}
			if (opened.global_settings)
			{
				// Left open rather than hidden: the offscreen platform does not activate a window shown a second
				// time, and an inactive window paints its selection and focus differently.
				if (view)
					view->restore(opened.window);
			}
			else if (opened.window && opened.window != g_main_window)
			{
				delete opened.window;
			}
		};

		if (!OpenTarget(target, &opened, &error))
		{
			cleanup(); // a page that is not there can still have opened its window
			return fail(error);
		}

		QWidget* window = opened.window;
		view = std::make_unique<ViewState>(window);
		window->raise();
		window->activateWindow();
		if (s_request.size.isValid())
			window->resize(s_request.size);
		Pump(settle_ms);

		bool clicked = false;
		bool ok = true;
		for (int i = 1; i < parts.size() && ok; i++)
		{
			const int eq = parts[i].indexOf('=');
			const QString action = (eq < 0 ? parts[i] : parts[i].left(eq)).trimmed();
			const QString value = eq < 0 ? QString() : parts[i].mid(eq + 1).trimmed();

			if (action == QLatin1String("focus"))
			{
				QWidget* widget = FindWidget(window, value, &error);
				ok = widget != nullptr;
				if (widget)
					Reveal(widget);
			}
			else if (action == QLatin1String("tab"))
			{
				ok = SelectTab(window, value, &error);
			}
			else if (action == QLatin1String("click"))
			{
				clicked = true;
				ok = Click(window, value, &error);
			}
			else if (action == QLatin1String("wait"))
			{
				settle_ms = value.toInt();
				s_modal_watcher->setSettle(settle_ms);
				Pump(settle_ms);
			}
			else if (action == QLatin1String("size"))
			{
				QSize size;
				ok = ParseSize(value, &size);
				if (ok)
					window->resize(size);
				else
					error = QStringLiteral("size wants WxH, got '%1'").arg(value);
			}
			else
			{
				ok = false;
				error = QStringLiteral("unknown action '%1'; use focus, tab, click, wait or size").arg(action);
			}
			Pump(100); // layout and repaint after each action
		}

		if (s_timed_out)
			return false; // the run is ending; the failure is already reported

		// What to grab: a modal dialog an action opened, else a window it opened, else the window asked for.
		QPixmap pixmap;
		QWidget* grabbed = window;
		QString note;
		if (ok)
		{
			if (!s_modal_watcher->pixmap.isNull() && clicked)
			{
				pixmap = s_modal_watcher->pixmap;
				note = QStringLiteral(" (modal '%1')").arg(s_modal_watcher->title);
			}
			else
			{
				if (!s_modal_watcher->pixmap.isNull())
					Say(QStringLiteral("warn %1: an unexpected modal dialog '%2' opened and was dismissed")
							.arg(shot.spec, s_modal_watcher->title));

				for (QWidget* widget : QApplication::topLevelWidgets())
				{
					const Qt::WindowType type = widget->windowType();
					if (widget->isVisible() && !before.contains(widget) && widget != window && type != Qt::Popup && type != Qt::ToolTip)
						grabbed = widget;
				}
				pixmap = grabbed->grab();
				if (grabbed != window)
					note = QStringLiteral(" (new window '%1')").arg(grabbed->windowTitle());
			}

			QDir().mkpath(QFileInfo(shot.path).absolutePath());
			if (pixmap.isNull())
			{
				ok = false;
				error = QStringLiteral("the window rendered nothing");
			}
			else if (!pixmap.save(shot.path, "PNG"))
			{
				ok = false;
				error = QStringLiteral("could not write %1").arg(shot.path);
			}
		}

		if (ok)
			Say(QStringLiteral("ok %1 %2 %3x%4%5").arg(shot.spec, shot.path).arg(pixmap.width()).arg(pixmap.height()).arg(note));

		cleanup();

		return ok ? true : fail(error);
	}

	void RunAll()
	{
		s_modal_watcher = new ModalWatcher();

		int failures = 0;
		for (const Shot& shot : s_request.shots)
		{
			if (!RunShot(shot))
				failures++;
			if (s_timed_out)
				return; // exit(2) is already on its way; later shots would run without an event loop
		}

		Say(failures ? QStringLiteral("done %1 of %2 shots failed").arg(failures).arg(s_request.shots.size()) :
					   QStringLiteral("done %1 shots").arg(s_request.shots.size()));
		QCoreApplication::exit(failures ? 1 : 0);
	}
} // namespace

void QtShot::RegisterDialog(const char* name, const char* description, DialogFactory factory)
{
	Dialogs().push_back({QString::fromLatin1(name), QString::fromUtf8(description), std::move(factory)});
}

bool QtShot::ParseArgument(QStringList::const_iterator& it, QStringList::const_iterator end)
{
	const QString flag = *it;
	if (!flag.startsWith(QLatin1String("-uishot")))
		return false;

	// Without these a stray run would flash windows on the user's desktop and use their real settings. It
	// has to stop here: main() shows the main window before Start() is reached.
	static bool s_guarded = false;
	if (!s_guarded)
	{
		s_guarded = true;
		const QStringList all = QCoreApplication::arguments();
		if (!all.contains(QLatin1String("-datapath")) || all.contains(QLatin1String("-portable")))
			Refuse(QStringLiteral("-uishot needs -datapath <dir> (and not -portable), or the shots use your real settings"));
		if (QGuiApplication::platformName() != QLatin1String("offscreen") && !all.contains(QLatin1String("-uishot-any-platform")))
			Refuse(QStringLiteral("-uishot opens windows and the platform is '%1', not offscreen; run with QT_QPA_PLATFORM=offscreen "
								  "or pass -uishot-any-platform")
					.arg(QGuiApplication::platformName()));
	}

	auto take = [&](int count, QStringList* values) {
		for (int i = 0; i < count; i++)
		{
			if (std::next(it) == end)
			{
				Say(QStringLiteral("fail %1 needs %2 parameter(s)").arg(flag).arg(count));
				s_request.error = true;
				return false;
			}
			values->push_back(*++it);
		}
		return true;
	};

	QStringList values;
	if (flag == QLatin1String("-uishot"))
	{
		if (take(2, &values))
			s_request.shots.push_back({values[0], QFileInfo(values[1]).absoluteFilePath()});
	}
	else if (flag == QLatin1String("-uishot-game"))
	{
		if (take(1, &values))
			s_request.game_path = values[0];
	}
	else if (flag == QLatin1String("-uishot-size"))
	{
		if (take(1, &values) && !ParseSize(values[0], &s_request.size))
		{
			Say(QStringLiteral("fail -uishot-size wants WxH, got '%1'").arg(values[0]));
			s_request.error = true;
		}
	}
	else if (flag == QLatin1String("-uishot-wait"))
	{
		if (take(1, &values))
			s_request.wait_ms = values[0].toInt();
	}
	else if (flag == QLatin1String("-uishot-any-platform"))
	{
		// only read by the guard above
	}
	else if (flag == QLatin1String("-uishot-timeout"))
	{
		if (take(1, &values))
			s_request.timeout_s = values[0].toInt();
	}
	else
	{
		Say(QStringLiteral("fail unknown flag %1").arg(flag));
		s_request.error = true;
	}
	return true;
}

void QtShot::Start()
{
	if (s_request.shots.empty() && !s_request.error)
		return;

	if (s_request.error || s_request.shots.empty())
	{
		QTimer::singleShot(0, qApp, [] { QCoreApplication::exit(2); });
		return;
	}

	s_context.game_path = s_request.game_path;

	// A thread that never finishes, or a dialog nothing can dismiss, must not hold the machine.
	QTimer::singleShot(s_request.timeout_s * 1000, qApp, [] {
		Say(QStringLiteral("fail timed out after %1 s").arg(s_request.timeout_s));
		s_timed_out = true;
		QCoreApplication::exit(2);
	});

	// The game list scan finishes on a worker thread and reaches the UI as a queued event, so the event loop
	// has not delivered it yet and connecting here cannot miss it. Queued again so the shots do not run
	// inside GameListWidget::onRefreshComplete, which still has work to do after emitting.
	GameListWidget* list = g_main_window->findChild<GameListWidget*>();
	if (list)
		QObject::connect(list, &GameListWidget::refreshComplete, qApp, [] { RunAll(); }, Qt::ConnectionType(Qt::QueuedConnection | Qt::SingleShotConnection));
	else
		QTimer::singleShot(0, qApp, [] { RunAll(); });
}
