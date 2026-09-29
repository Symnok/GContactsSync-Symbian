// GContactsSync - a Google Contacts CardDAV client for Symbian^3 / Anna / Belle.
// Copyright (C) 2026 - GPL-3.0-or-later, see LICENSE.
//
// Startup is written defensively on purpose: there is no debugger on the phone, and a failure in
// here shows as nothing but a blank screen, so QML errors are logged and painted rather than
// swallowed.
#include <QtCore/QDebug>
#include <QtCore/QUrl>
#include <QtDeclarative/QDeclarativeContext>
#include <QtDeclarative/QDeclarativeError>
#include <QtDeclarative/QDeclarativeView>
#include <QtGui/QApplication>

#include "appcontroller.h"

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    app.setApplicationName(QLatin1String("GContactsSync"));
    app.setOrganizationName(QLatin1String("GContactsSync"));

    AppController controller;

    QDeclarativeView view;
    view.setResizeMode(QDeclarativeView::SizeRootObjectToView);
    view.rootContext()->setContextProperty(QLatin1String("app"), &controller);
    view.setSource(QUrl(QLatin1String("qrc:/qml/main.qml")));

    if (view.status() == QDeclarativeView::Error) {
        const QList<QDeclarativeError> errors = view.errors();
        for (int i = 0; i < errors.size(); ++i)
            qWarning() << "QML:" << errors.at(i).toString();
    }

#ifdef Q_OS_SYMBIAN
    view.setAttribute(Qt::WA_LockPortraitOrientation, true);
    view.showFullScreen();
#else
    view.resize(360, 640);
    view.show();
#endif

    return app.exec();
}
