// GContactsSync - a Google Contacts CardDAV client for Symbian^3 / Anna / Belle.
// Copyright (C) 2026 - GPL-3.0-or-later, see LICENSE.
//
// The phonebook half of the sync, in native Symbian C++ over CContactDatabase.
//
// This was Qt Mobility's QContactManager to begin with, and on the device it did not work: of 29
// contacts 11 saves were accepted, 18 rejected, and none of them appeared in Contacts at all.
// SymGContacts (https://github.com/Symnok/SymGContacts), which syncs Google contacts to the same
// phones over the People API, avoids QContactManager for the same reason and says so plainly -
// compatibility problems on real hardware - and reaches the phonebook through cntmodel.lib
// instead. So does this now.
//
// Two consequences of going native, both deliberate:
//
//   * Every call into the contacts API is wrapped in TRAPD. Symbian leaves are not C++ exceptions
//     and must never cross back into Qt code; a corrupt contact in the database otherwise takes
//     the whole app down with it.
//   * Field labels (mobile / home / work / fax) are vCard mappings added to a field's content
//     type, which is exactly how the native Contacts application stores them - so a number synced
//     here is labelled the same as one typed in by hand.
//
// Symbian has no per-app contact store: there is one shared phonebook holding the user's real
// contacts. So the rule this class enforces is narrow and absolute:
//
//     this app knows which contacts are its own, and NOTHING here ever reads, updates or deletes
//     a contact that is not.
//
// Ownership is recorded twice. The dependable index is a Google UID -> contact id map of our own,
// kept beside the rest of the sync state: exact, private, and no scan needed. Because that file
// could be lost while the contacts remain, each contact also carries a "[GID:<uid>]" line at the
// top of its note, which is how SymGContacts marks its own and is the one tag known to survive on
// these devices. The line is stripped when the note is read back, so it never reaches Google and
// never counts as an edit.
#ifndef PHONECONTACTS_H
#define PHONECONTACTS_H

#include <QtCore/QMap>
#include <QtCore/QString>

#include "contactstore.h"

class CContactDatabase;
class CContactItem;

class PhoneContacts : public IContactStore
{
public:
    /// dataDir is where the UID -> contact id map lives, alongside the sync state.
    explicit PhoneContacts(const QString &dataDir);
    ~PhoneContacts();

    /// Whether the phonebook could be opened. When false, error() says why and everything fails.
    bool isReady() const { return m_db != 0; }
    QString error() const { return m_error; }

    /// What a bug report needs when a sync will not stick: whether the database opened, how many
    /// contacts it holds, how many are ours, and why the last write failed.
    QString diagnostics();

    /// Writes how the fields of a few synced contacts, a few hand-made ones and the system
    /// template are stored (types, mapping, label, flags; values as lengths only) to path.
    /// Returns path, or an empty string when the file could not be written.
    QString dumpFields(const QString &path);

    /// Whether the contacts this app owns were written in the current field layout. Up to 1.0.13
    /// fields were built by hand and Contacts showed little more than the names; when this is
    /// false the caller makes the next download a full one, which rewrites every contact in place.
    /// Card templates other than the system one, as "id: name" lines. The Contacts application
    /// never creates these, and one was found on a phone whose Contacts hung on opening any
    /// contact - including ones made by hand after everything else was deleted.
    QString strayTemplates();

    /// Deletes those card templates, recovers the database if it reports damage, and compacts
    /// it. The only thing in this class that touches items this app does not own, and it runs
    /// only when the user confirms it. Returns what was done.
    QString repairPhonebook();

    bool layoutIsCurrent() const;
    void markLayoutCurrent();

    // IContactStore
    bool write(const VCardContact &c, QString *error);
    bool removeByUid(const QString &uid);
    bool readByUid(const QString &uid, VCardContact *out);
    bool readAll(QList<VCardContact> *out, QString *error);

private:
    // The leaving halves, each called only from inside a TRAPD.
    void openDatabaseL();
    void writeL(const VCardContact &c);
    void readOneL(quint32 contactId, VCardContact *out);
    void readAllL(QList<VCardContact> *out);
    void removeL(quint32 contactId);

    /// The contact id this app holds for a UID, or 0. Checks the map first, then the note tag.
    quint32 idForUid(const QString &uid);
    quint32 findByNoteTagL(const QString &uid);
    void buildTagIndexL();

    bool loadOwned();
    bool saveOwned() const;
    QString ownedPath() const;
    QString layoutPath() const;

    static QString errorText(int symbianError);

    CContactDatabase *m_db;
    QString m_dataDir;
    QString m_error;
    QString m_lastWriteError;

    /// Google UID -> TContactItemId. This is what "ours" means.
    QMap<QString, quint32> m_owned;

    /// The phonebook's system template: the Contacts application's own definition of every field.
    /// Each field this app writes is a copy of the matching one, so Contacts recognises it.
    CContactItem *m_template;

    /// Note tag -> contact id, from one pass over the phonebook. Without it a first sync scanned
    /// the whole phonebook once per contact. Rebuilt when older than a minute.
    QMap<QString, quint32> m_tagIndex;
    qint64 m_tagIndexBuiltMs;
};

#endif // PHONECONTACTS_H
