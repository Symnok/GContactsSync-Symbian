// GContactsSync - a Google Contacts CardDAV client for Symbian^3 / Anna / Belle.
// Copyright (C) 2026 - GPL-3.0-or-later, see LICENSE.
//
// Turning Symbian's startup list entry on and off at runtime.
//
// The Startup List Manager launches anything that has a STARTUP_ITEM_INFO resource in its import
// directory, \private\101f875a\import\. Writing into another application's private directory needs
// the AllFiles capability, which a self-signed package cannot be granted - so on a stock phone this
// cannot work, and the app says so.
//
// On a phone whose platform security has been patched (the usual Open4all / hacked installserver
// arrangement) the file server's capability checks are bypassed and the write simply succeeds.
// That is why AllFiles is deliberately NOT declared in the .pro: declaring it would make the
// package refuse to install on an unmodified phone. Instead the write is attempted and the result
// reported, so the same package works everywhere and the feature is available where the phone
// allows it.
//
// The resource itself cannot be produced at runtime - it is a compiled Symbian resource - so it
// ships in the package at \resource\apps\ and is copied into place from there.
#ifndef AUTOSTART_H
#define AUTOSTART_H

#include <QtCore/QString>

namespace Autostart
{
/// Whether the startup entry is currently in place. This reads the file system rather than a
/// setting, so it cannot disagree with what the phone will actually do at boot.
bool isEnabled();

/// Puts the entry in place. False on failure, with *error saying why in words worth showing.
bool enable(QString *error);

/// Removes it. False on failure.
bool disable(QString *error);

/// Where the entry goes: C:\private\101f875a\import\GContactsSync.rsc
QString entryPath();

/// The copy shipped in the package, which enable() copies from.
QString templatePath();
}

#endif // AUTOSTART_H
