// GContactsSync - a Google Contacts CardDAV client for Symbian^3 / Anna / Belle.
// Copyright (C) 2026 - GPL-3.0-or-later, see LICENSE.
//
// Everything the app has to remember between syncs:
//
//   etags    path on Google -> etag, so a download can ask only for what changed
//   hrefs    Google UID     -> path, so an upload PUTs over the right card
//   hashes   Google UID     -> fingerprint of the contact as it reads back off the phonebook,
//                              so an upload knows which contacts the user actually edited
//   cards    Google UID     -> the last card Google sent, verbatim, one file each
//
// The presence of any etags is also what "have we ever synced?" means: no etags, so the next
// download is a full one.
//
// The card cache is what makes an upload safe. A CardDAV PUT replaces the whole card and a phone
// contact cannot hold everything a Google contact can - the photo in its original encoding,
// CATEGORIES, IMPP handles, the X- properties Google keeps for itself. Uploading a card built only
// from what the phonebook holds would delete all of it, so VCardWriter::patch rewrites just the
// properties the phone owns inside the stored card.
//
// The directory is passed in rather than discovered: this keeps the core clear of QtGui
// (QDesktopServices), so the desktop harness can point it at a scratch folder.
#ifndef SYNCSTATE_H
#define SYNCSTATE_H

#include <QtCore/QMap>
#include <QtCore/QString>

class SyncState
{
public:
    explicit SyncState(const QString &dataDir);

    QMap<QString, QString> etags;    // href -> etag
    QMap<QString, QString> hrefs;    // uid  -> href
    QMap<QString, QString> hashes;   // uid  -> fingerprint

    bool hasSynced() const { return !etags.isEmpty(); }

    bool load();
    bool save() const;

    /// Forgets every record, so the next download starts from scratch. Contacts already on the
    /// phone are left alone.
    void clear();

    /// The card Google last sent for a UID, or a null string when there is none.
    QString readCard(const QString &uid) const;
    void writeCard(const QString &uid, const QString &card) const;
    void deleteCard(const QString &uid) const;

    /// Drops every record for a contact that no longer exists on Google.
    void forget(const QString &uid, const QString &href);

    /// The UID a contact path belongs to. Google names every card <uid>.vcf; the recorded map is
    /// authoritative and this is the fallback.
    static QString uidFromHref(const QString &href);

    QString dataDir() const { return m_dir; }

private:
    QString mapPath(const QString &name) const;
    QString cardPath(const QString &uid) const;
    bool ensureDirs() const;

    QString m_dir;
};

#endif // SYNCSTATE_H
