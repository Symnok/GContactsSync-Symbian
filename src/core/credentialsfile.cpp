// GContactsSync - a Google Contacts CardDAV client for Symbian^3 / Anna / Belle.
// Copyright (C) 2026 - GPL-3.0-or-later, see LICENSE.
#include "credentialsfile.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QTextStream>

namespace {

/// What is wrong with what was read, or a null string. The two shape checks are worth making here
/// rather than letting them come back as a 401 from Google two taps later - a password that is not
/// 16 characters is almost always the account password by mistake.
QString describe(const CredentialsFileResult &r)
{
    if (r.isComplete()) {
        if (!r.email.contains(QLatin1Char('@')))
            return QLatin1String("the login line does not look like an email address");
        if (r.password.size() != 16)
            return QString::fromLatin1("the password is %1 characters; a Google App Password is 16")
                       .arg(r.password.size());
        return QString();
    }

    if (r.email.isEmpty() && r.password.isEmpty())
        return QLatin1String("no \"login:\" or \"password:\" line found");
    if (r.email.isEmpty())
        return QLatin1String("no \"login:\" line found");
    return QLatin1String("no \"password:\" line found");
}

} // anonymous namespace

namespace CredentialsFile {

QString defaultFileName()
{
    return QLatin1String("gcontactssync.txt");
}

QStringList searchPaths()
{
    QStringList dirs;
#ifdef Q_OS_SYMBIAN
    // Where a file copied over USB or Bluetooth actually ends up. E: is the memory card on every
    // Symbian^3 device, F: the mass memory on some, C: phone memory.
    dirs << QLatin1String("E:/") << QLatin1String("E:/Data/")
         << QLatin1String("F:/") << QLatin1String("F:/Data/")
         << QLatin1String("C:/Data/");
#else
    dirs << QDir::currentPath() + QLatin1Char('/') << QDir::homePath() + QLatin1Char('/');
#endif

    QStringList paths;
    for (int i = 0; i < dirs.size(); ++i) paths.append(dirs.at(i) + defaultFileName());
    return paths;
}

CredentialsFileResult parse(const QString &text)
{
    CredentialsFileResult r;

    if (text.isEmpty()) {
        r.problem = QLatin1String("the file is empty");
        return r;
    }

    const QStringList lines = text.split(QLatin1Char('\n'));
    for (int i = 0; i < lines.size(); ++i) {
        const QString line = lines.at(i).trimmed();
        if (line.isEmpty() || line.at(0) == QLatin1Char('#') || line.at(0) == QLatin1Char(';'))
            continue;

        // The first separator only. An email address contains neither ':' nor '=', but splitting
        // on the last would break the moment a value did.
        const int colon = line.indexOf(QLatin1Char(':'));
        const int equals = line.indexOf(QLatin1Char('='));
        int split;
        if (colon < 0) split = equals;
        else if (equals < 0) split = colon;
        else split = qMin(colon, equals);
        if (split <= 0) continue;

        const QString key = line.left(split).trimmed().toLower();
        const QString value = line.mid(split + 1).trimmed();
        if (value.isEmpty()) continue;

        if (key == QLatin1String("login") || key == QLatin1String("email")
            || key == QLatin1String("user") || key == QLatin1String("username")
            || key == QLatin1String("account")) {
            r.email = value;
        } else if (key == QLatin1String("password") || key == QLatin1String("pass")
                   || key == QLatin1String("apppassword")
                   || key == QLatin1String("app password")) {
            // Google shows an App Password as four groups of four. Copying it in that shape is
            // the obvious thing to do, and is not what Google accepts.
            r.password = QString(value).remove(QLatin1Char(' ')).remove(QLatin1Char('\t'));
        }
    }

    r.problem = describe(r);
    return r;
}

CredentialsFileResult loadFrom(const QString &path)
{
    CredentialsFileResult r;

    QFile file(path);
    if (!file.exists()) return r;   // nothing found; problem stays null

    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        r.problem = QLatin1String("could not open ") + QFileInfo(path).fileName();
        r.source = path;
        return r;
    }

    QTextStream in(&file);
    r = parse(in.readAll());
    r.source = path;
    return r;
}

CredentialsFileResult load()
{
    const QStringList paths = searchPaths();
    for (int i = 0; i < paths.size(); ++i) {
        const CredentialsFileResult r = loadFrom(paths.at(i));
        // A file that exists but is wrong is reported rather than skipped: silently moving on
        // would look exactly like having no file at all.
        if (!r.source.isEmpty()) return r;
    }
    return CredentialsFileResult();
}

} // namespace CredentialsFile
