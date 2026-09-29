// GContactsSync - a Google Contacts CardDAV client for Symbian^3 / Anna / Belle.
// Copyright (C) 2026 - GPL-3.0-or-later, see LICENSE.
#include "syncstate.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QTextStream>
#include <QtCore/QUrl>

namespace {

const char * const kEtagsFile = "etags.tsv";
const char * const kHrefsFile = "hrefs.tsv";
const char * const kHashesFile = "hashes.tsv";
const char * const kCardsDir = "cards";

/// A tab or newline in a key or value would break the format. None can appear in an href, an etag,
/// a UID or a fingerprint, so they are dropped rather than escaped.
QString clean(const QString &s)
{
    QString out = s;
    out.replace(QLatin1Char('\t'), QLatin1Char(' '));
    out.replace(QLatin1Char('\r'), QLatin1Char(' '));
    out.replace(QLatin1Char('\n'), QLatin1Char(' '));
    return out;
}

bool readMap(const QString &path, QMap<QString, QString> *map)
{
    map->clear();

    QFile file(path);
    if (!file.exists()) return true;   // nothing synced yet
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return false;

    QTextStream in(&file);
    in.setCodec("UTF-8");
    while (!in.atEnd()) {
        const QString line = in.readLine();
        const int tab = line.indexOf(QLatin1Char('\t'));
        if (tab <= 0) continue;
        map->insert(line.left(tab), line.mid(tab + 1));
    }
    return true;
}

bool writeMap(const QString &path, const QMap<QString, QString> &map)
{
    // Written to a temporary and renamed: a sync interrupted by a flat battery then leaves the
    // previous state intact rather than half a file.
    const QString tempPath = path + QLatin1String(".new");

    QFile file(tempPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) return false;

    QTextStream out(&file);
    out.setCodec("UTF-8");
    for (QMap<QString, QString>::const_iterator it = map.constBegin();
         it != map.constEnd(); ++it) {
        out << clean(it.key()) << '\t' << clean(it.value()) << '\n';
    }
    out.flush();
    const bool ok = file.error() == QFile::NoError;
    file.close();
    if (!ok) { QFile::remove(tempPath); return false; }

    QFile::remove(path);
    return QFile::rename(tempPath, path);
}

/// A file name that is safe on a FAT volume and unique for this UID. The readable part is
/// truncated, so a short hash of the whole key keeps two long UIDs sharing a prefix apart.
QString safeFileName(const QString &key)
{
    QString out;
    for (int i = 0; i < key.size() && out.size() < 32; ++i) {
        const QChar ch = key.at(i);
        out += (ch.isLetterOrNumber() && ch.unicode() < 128) ? ch : QLatin1Char('_');
    }

    quint32 hash = 2166136261u;
    for (int i = 0; i < key.size(); ++i) {
        hash ^= key.at(i).unicode();
        hash *= 16777619u;
    }

    return out + QLatin1Char('-') + QString::number(hash, 16).rightJustified(8, QLatin1Char('0'))
         + QLatin1String(".vcf");
}

} // anonymous namespace

SyncState::SyncState(const QString &dataDir)
    : m_dir(dataDir)
{
    if (!m_dir.endsWith(QLatin1Char('/'))) m_dir += QLatin1Char('/');
}

QString SyncState::mapPath(const QString &name) const
{
    return m_dir + name;
}

QString SyncState::cardPath(const QString &uid) const
{
    return m_dir + QLatin1String(kCardsDir) + QLatin1Char('/') + safeFileName(uid);
}

bool SyncState::ensureDirs() const
{
    QDir dir(m_dir);
    if (!dir.exists() && !dir.mkpath(QLatin1String("."))) return false;
    return dir.exists(QLatin1String(kCardsDir)) || dir.mkpath(QLatin1String(kCardsDir));
}

bool SyncState::load()
{
    return readMap(mapPath(QLatin1String(kEtagsFile)), &etags)
        && readMap(mapPath(QLatin1String(kHrefsFile)), &hrefs)
        && readMap(mapPath(QLatin1String(kHashesFile)), &hashes);
}

bool SyncState::save() const
{
    if (!ensureDirs()) return false;
    return writeMap(mapPath(QLatin1String(kEtagsFile)), etags)
        && writeMap(mapPath(QLatin1String(kHrefsFile)), hrefs)
        && writeMap(mapPath(QLatin1String(kHashesFile)), hashes);
}

void SyncState::clear()
{
    etags.clear();
    hrefs.clear();
    hashes.clear();

    QFile::remove(mapPath(QLatin1String(kEtagsFile)));
    QFile::remove(mapPath(QLatin1String(kHrefsFile)));
    QFile::remove(mapPath(QLatin1String(kHashesFile)));

    QDir cards(m_dir + QLatin1String(kCardsDir));
    if (!cards.exists()) return;

    const QStringList files = cards.entryList(QStringList() << QLatin1String("*.vcf"), QDir::Files);
    for (int i = 0; i < files.size(); ++i) cards.remove(files.at(i));
}

// A card is stored as raw UTF-8 bytes, deliberately NOT through a text stream: QIODevice::Text
// rewrites line endings, and a vCard's CRLF framing is part of the data. Patching an upload
// depends on this file being exactly what Google sent, down to the bytes.
QString SyncState::readCard(const QString &uid) const
{
    if (uid.isEmpty()) return QString();

    QFile file(cardPath(uid));
    if (!file.open(QIODevice::ReadOnly)) return QString();

    return QString::fromUtf8(file.readAll());
}

void SyncState::writeCard(const QString &uid, const QString &card) const
{
    if (uid.isEmpty() || card.isEmpty()) return;
    if (!ensureDirs()) return;

    QFile file(cardPath(uid));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return;

    file.write(card.toUtf8());
    // A card that cannot be cached only costs fidelity on the next upload of that one contact,
    // which is not worth failing a whole sync over.
}

void SyncState::deleteCard(const QString &uid) const
{
    if (uid.isEmpty()) return;
    QFile::remove(cardPath(uid));
}

void SyncState::forget(const QString &uid, const QString &href)
{
    if (!href.isEmpty()) etags.remove(href);
    if (uid.isEmpty()) return;
    hrefs.remove(uid);
    hashes.remove(uid);
}

QString SyncState::uidFromHref(const QString &href)
{
    if (href.isEmpty()) return QString();

    const int slash = href.lastIndexOf(QLatin1Char('/'));
    QString name = slash >= 0 ? href.mid(slash + 1) : href;
    if (name.endsWith(QLatin1String(".vcf"), Qt::CaseInsensitive)) name.chop(4);

    return QUrl::fromPercentEncoding(name.toLatin1());
}
