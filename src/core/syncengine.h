// GContactsSync - a Google Contacts CardDAV client for Symbian^3 / Anna / Belle.
// Copyright (C) 2026 - GPL-3.0-or-later, see LICENSE.
//
// The two directions of sync, with the same shape the Windows Phone and Windows 10 Mobile versions
// have:
//
//   download (Google -> phone)   manually, and on a timer of at least 15 minutes. The first run
//                                fetches everything; every run after that compares stored etags
//                                against the server's and touches only what moved.
//
//   upload (phone -> Google)     manually only, never on a timer. Contacts whose fingerprint has
//                                not changed since the last download are left alone.
//
// Platform-free: it talks to the phonebook through IContactStore, so tools/gcontactssync-cli can
// run both directions against a real Google account with an in-memory store standing in for the
// phone.
#ifndef SYNCENGINE_H
#define SYNCENGINE_H

#include <QtCore/QObject>
#include <QtCore/QString>

class CardDavClient;
class IContactStore;
class SyncState;

class SyncOutcome
{
public:
    SyncOutcome()
        : success(false), wasFullSync(false), nothingToDo(false)
        , written(0), removed(0), uploaded(0), conflicts(0), failed(0), skipped(0) {}

    bool success;
    bool wasFullSync;
    bool nothingToDo;

    int written;     // contacts created or updated on the phone
    int removed;     // contacts deleted from the phone
    int uploaded;    // contacts sent to Google
    int conflicts;   // contacts Google had changed in the meantime
    int failed;
    int skipped;

    /// One line, ready to show to the user.
    QString message;
    /// Set when success is false; safe to show as it is.
    QString error;
};

class SyncEngine : public QObject
{
    Q_OBJECT

public:
    explicit SyncEngine(QObject *parent = 0);

    SyncOutcome download(CardDavClient *client, IContactStore *store, SyncState *state);
    SyncOutcome upload(CardDavClient *client, IContactStore *store, SyncState *state);

signals:
    void progress(const QString &message);

private:
    bool fullDownload(CardDavClient *client, IContactStore *store, SyncState *state,
                      SyncOutcome *outcome);
    bool incrementalDownload(CardDavClient *client, IContactStore *store, SyncState *state,
                             SyncOutcome *outcome);

    /// Writes one downloaded card to the phone and records what the next sync in either direction
    /// will need: the card itself, its path, and the fingerprint of the result as the phonebook
    /// actually stored it. False when that contact could not be written.
    bool storeCard(IContactStore *store, SyncState *state, const class FetchedCard &card);

    void report(const QString &message);

    /// Why the first contact that would not save failed. Reported with the outcome, because a
    /// download that stores nothing is useless without the reason.
    QString m_firstWriteError;

    static QString contacts(int count);
    static QString describeDownload(const SyncOutcome &outcome);
};

#endif // SYNCENGINE_H
