// GContactsSync - a Google Contacts CardDAV client for Symbian^3 / Anna / Belle.
// Copyright (C) 2026 - GPL-3.0-or-later, see LICENSE.
#include "autostart.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>

namespace {

/// The Startup List Manager's own private directory, on the system drive. Its import subdirectory
/// is scanned at boot.
const char * const kImportDir = "C:/private/101f875a/import";

/// What the entry is called once it is there. Any name will do; the app's own keeps it obvious
/// which application put it there, and makes it ours to delete again.
const char * const kEntryName = "GContactsSync.rsc";

/// Installed alongside the app by the package.
const char * const kTemplateName = "GContactsSync_startup.rsc";

/// The drive the app was installed on, e.g. "C:" or "E:".
QString installDrive()
{
    const QString path = QCoreApplication::applicationFilePath();
    if (path.size() >= 2 && path.at(1) == QLatin1Char(':'))
        return path.left(2);
    return QLatin1String("C:");
}

} // anonymous namespace

namespace Autostart {

QString entryPath()
{
    return QLatin1String(kImportDir) + QLatin1Char('/') + QLatin1String(kEntryName);
}

QString templatePath()
{
    return installDrive() + QLatin1String("/resource/apps/") + QLatin1String(kTemplateName);
}

bool isEnabled()
{
    return QFile::exists(entryPath());
}

bool enable(QString *error)
{
    if (isEnabled()) return true;

    const QString source = templatePath();
    if (!QFile::exists(source)) {
        *error = QObject::tr("The startup entry is missing from the installation (%1). "
                             "Reinstalling the app should restore it.").arg(source);
        return false;
    }

    // The import directory usually exists already; creating it is for the case where nothing has
    // ever registered for startup on this phone.
    QDir().mkpath(QLatin1String(kImportDir));

    if (!QFile::copy(source, entryPath())) {
        // Almost always a stock phone refusing the write. Nothing is wrong with the app, and
        // saying so plainly is more use than an error code.
        *error = QObject::tr("This phone did not allow the startup entry to be created.\n\n"
                             "Writing into another application's private folder needs the AllFiles "
                             "capability, which a self-signed app cannot have. It works only on a "
                             "phone whose platform security has been patched.");
        return false;
    }
    return true;
}

bool disable(QString *error)
{
    if (!isEnabled()) return true;

    if (!QFile::remove(entryPath())) {
        *error = QObject::tr("The startup entry could not be removed (%1).").arg(entryPath());
        return false;
    }
    return true;
}

} // namespace Autostart
