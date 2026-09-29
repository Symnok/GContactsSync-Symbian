// GContactsSync - a Google Contacts CardDAV client for Symbian^3 / Anna / Belle.
// Copyright (C) 2026 - GPL-3.0-or-later, see LICENSE.
//
// Reading the account out of a text file, so a 16-character App Password does not have to be
// typed on a phone keypad every time something is tested.
//
// Two lines, nothing else:
//
//     login: someone@gmail.com
//     password: abcdefghijklmnop
//
// This only ever reads. There is deliberately no matching "save": writing an App Password into a
// world-readable file on the memory card would be a worse deal than typing it. The app's own copy
// goes in its private QSettings.
#ifndef CREDENTIALSFILE_H
#define CREDENTIALSFILE_H

#include <QtCore/QString>
#include <QtCore/QStringList>

class CredentialsFileResult
{
public:
    QString email;
    QString password;
    /// Why the file was not usable, ready to show to the user. Null when it was.
    QString problem;
    /// Which file this came from, for telling the user what was read.
    QString source;

    bool isComplete() const { return !email.isEmpty() && !password.isEmpty(); }
};

namespace CredentialsFile
{
/// What the app looks for, in each of the places searchPaths() lists.
QString defaultFileName();

/// Where the app looks, in order. On the phone these are the memory card and phone memory roots
/// and their Data folders - somewhere a file copied over USB actually lands.
QStringList searchPaths();

/// Reads the two lines. Deliberately forgiving about what is easy to get wrong when typing a file
/// for a phone: ':' or '=' as the separator, any spacing around it, any case for the key, blank
/// lines, '#' or ';' comments, and CRLF or LF.
CredentialsFileResult parse(const QString &text);

/// The first readable account file from searchPaths(). isComplete() is false when none was found,
/// and problem is then null - not finding one is the ordinary case.
CredentialsFileResult load();

/// One named file.
CredentialsFileResult loadFrom(const QString &path);
}

#endif // CREDENTIALSFILE_H
