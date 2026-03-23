// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2023 Droidian Project
// Copyright (C) 2024 Furi Labs
//
// Authors:
// Bardia Moshiri <fakeshell@bardia.tech>
// Erik Inkinen <erik.inkinen@gmail.com>
// Alexander Rutz <alex@familyrutz.com>
// Joaquin Philco <joaquinphilco@gmail.com>

#include <QApplication>
#include <QIcon>
#include <QFont>
#include <QSystemTrayIcon>
#include <QMenu>
#include <QQmlEngine>
#include "singleinstance.h"
#include "appcontroller.h"
#include "accelreader.h"
#include "whitebalancecontroller.h"

int main(int argc, char *argv[])
{
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    QCoreApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
#endif
    QApplication app(argc, argv);
    app.setOrganizationName("FuriCam");
    app.setOrganizationDomain("github.io");

    SingleInstance singleInstance;
    if (!singleInstance.listen("FuriCamApp")) {
        qDebug() << "Application already running";
        return 0;
    }

    qmlRegisterType<AccelReader>("FuriCam", 1, 0, "AccelReader");
    qmlRegisterType<WhiteBalanceController>("FuriCam", 1, 0, "WhiteBalanceController");

    QIcon::setThemeName("default");
    QIcon::setThemeSearchPaths(QStringList("/usr/share/icons"));

    const QFont cantarell = QFont("Cantarell");
    app.setFont(cantarell);

    AppController appController(app);

    QSystemTrayIcon trayIcon(QIcon("/usr/share/icons/furicam.svg"), &app);
    QMenu trayMenu;
    QAction quitAction("Quit");
    QObject::connect(&quitAction, &QAction::triggered, &app, &QCoreApplication::quit);
    trayMenu.addAction(&quitAction);
    trayIcon.setContextMenu(&trayMenu);
    trayIcon.show();

    QObject::connect(&singleInstance, &SingleInstance::showWindow, &appController, &AppController::showWindow);

    appController.initialize();
    appController.initializeSettings();
    appController.createDirectories();
    appController.restartGpsIfNeeded();

    return app.exec();
}
