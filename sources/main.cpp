/**
 * Copyright 2021, 2022 Lyubomir Filipov
 *
 * This file is part of Mattermost-QT.
 *
 * Mattermost-QT is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Mattermost-QT is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with Mattermost-QT. if not, see https://www.gnu.org/licenses/.
 */

#include <algorithm>
#include <memory>
#include <QApplication>
#include <QFont>
#include <QMenu>
#include <QSettings>
#include <QSystemTrayIcon>

#include "login/LoginDialog.h"
#include "mainwindow.h"
#include "backend/Backend.h"
#include "backend/CustomEmojiService.h"
#include "config/Config.h"
#include "navigation/NavigationUiController.h"
#include "Settings.h"
#include "ui/OverlayScrollBarManager.h"
#include "ui/SplitterHandleManager.h"

namespace Mattermost {

class MattermostApplication: public QApplication {
public:
	MattermostApplication (int& argc, char *argv[]);

	void openLoginWindow ();
	void showWindow ();
	void toggleShowWindow ();
	void reopen ();
	void quitApplication ();
	void closeAllThreadWindows ();
private:
	std::unique_ptr<MainWindow>			mainWindow;
	// Declare the context menu before the tray icon: members are destroyed in
	// reverse declaration order, so the menu is destroyed *after* the icon.
	// QSystemTrayIcon only stores a raw pointer to its context menu and does not
	// own it, so destroying the menu first would leave the icon holding a
	// dangling pointer that the platform tray code can dereference on shutdown
	// (use-after-free -> SIGSEGV on exit).
	std::unique_ptr<QMenu>				trayIconMenu;
	std::unique_ptr<QSystemTrayIcon> 	trayIcon;
	Backend								backend;
	LoginDialog*						loginDialog;
	QWidget*							currentWindow;
};

inline MattermostApplication::MattermostApplication (int& argc, char *argv[])
:QApplication (argc, argv)
,trayIconMenu (std::make_unique<QMenu> (nullptr))
,trayIcon (std::make_unique<QSystemTrayIcon> (QIcon(":/icons/img/icon0.ico"), nullptr))
,currentWindow (nullptr)
{
    OverlayScrollBarManager::install(*this);
    SplitterHandleManager::install(*this);
    (void)CustomEmojiService::instance(backend);

    Config::init ();
	trayIcon->setToolTip(tr("Mattermost Qt"));
	trayIcon->setContextMenu (trayIconMenu.get());
	trayIcon->show();

	connect (trayIcon.get(), &QSystemTrayIcon::messageClicked, this, &MattermostApplication::showWindow);

	connect (trayIcon.get(), &QSystemTrayIcon::activated, [this] (QSystemTrayIcon::ActivationReason reason) {
		if (reason == QSystemTrayIcon::Trigger) {
			toggleShowWindow ();
		}
	});

	trayIconMenu->addAction ("Open Mattermost", this, &MattermostApplication::showWindow);
	trayIconMenu->addAction ("Quit", this, &MattermostApplication::quitApplication);
	qApp->setQuitOnLastWindowClosed(false);

	// Guarantee the process always exits cleanly on Quit: tear down live
	// connections (WebSocket, HTTP, timers) deterministically before the event
	// loop ends, regardless of which path triggered the quit.
	connect (this, &QCoreApplication::aboutToQuit, [this] {
		closeAllThreadWindows ();
		backend.shutdown ();
	});
}

void MattermostApplication::quitApplication ()
{
	// Close detached thread windows and shut the backend down first so no
	// native window, reconnect/heartbeat timer or open socket can keep the
	// process alive, then exit the event loop.
	closeAllThreadWindows ();
	backend.shutdown ();
	QApplication::quit ();
}

void MattermostApplication::closeAllThreadWindows ()
{
	if (mainWindow) {
		NavigationUiController::instance (*mainWindow).closeAllThreadWindows ();
	}
}

void MattermostApplication::openLoginWindow ()
{
	loginDialog = new LoginDialog (nullptr, backend);
	loginDialog->open();
	currentWindow = loginDialog;

	connect (loginDialog, &LoginDialog::accepted, [this] {
		//create Main Window and open it, after successful login
		loginDialog = nullptr;
		mainWindow = std::make_unique<MainWindow> (nullptr, *trayIcon, backend);
        mainWindow->installRealtimeUiSync();
		mainWindow->show();
		currentWindow = mainWindow.get();
	});
}

inline void MattermostApplication::showWindow ()
{
	if (currentWindow && !currentWindow->isVisible()) {
		currentWindow->show ();
	}
}

inline void MattermostApplication::toggleShowWindow ()
{
	if (!currentWindow) {
		return;
	}

	if (currentWindow->isVisible()) {
		currentWindow->hide ();
	} else {
		currentWindow->show ();
	}
}

} /* namespace Mattermost */

namespace {

void applyUiFontScale(QApplication& app)
{
	const QSettings settings;
	int percent = settings.value(UI_FONT_SCALE_PERCENT,
	                             UI_FONT_SCALE_PERCENT_DEFAULT).toInt();
	percent = std::clamp(percent,
	                     UI_FONT_SCALE_PERCENT_MIN,
	                     UI_FONT_SCALE_PERCENT_MAX);
	if (percent == 100) {
		return;
	}

	const qreal factor = percent / 100.0;
	QFont font = app.font();
	font.setPointSizeF(font.pointSizeF() * factor);
	app.setFont(font);
}

} // namespace

int main( int argc, char *argv[])
{
	QCoreApplication::setOrganizationName("mattermost-native");
	QCoreApplication::setApplicationName("Mattermost");
	QGuiApplication::setHighDpiScaleFactorRoundingPolicy(Qt::HighDpiScaleFactorRoundingPolicy::Round);

	Mattermost::MattermostApplication app (argc, argv);
	applyUiFontScale(app);
	app.openLoginWindow ();
	return app.exec();
}

