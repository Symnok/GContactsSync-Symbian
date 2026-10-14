// GContactsSync - a Google Contacts CardDAV client for Symbian^3 / Anna / Belle.
// Copyright (C) 2026 - GPL-3.0-or-later, see LICENSE.
//
// Google Contacts over CardDAV, with a Google App Password as Basic auth. No OAuth client, no
// Cloud project, no People API - which is the point of the app this one is a port of.
//
// Requests are issued through QNetworkAccessManager and waited for on a nested event loop, so the
// calls below read linearly. That is a deliberate choice for this platform: the alternative, a
// worker thread, would put the phonebook access (QtContacts, which wraps thread-affine Symbian
// contact sessions) on the wrong thread. The UI stays alive because a nested loop still delivers
// its events; SyncEngine holds a busy flag so a second sync cannot start on top of the first.
//
// Unlike the Windows Phone version of this app there is no bundled OpenSSL: Symbian^3 / Anna /
// Belle negotiate modern TLS through Qt's own stack. The CA bundle shipped in the resources is
// still handed to QSslConfiguration, because a 2011 device's root store predates the Google Trust
// Services roots that www.google.com chains to today.
#ifndef CARDDAVCLIENT_H
#define CARDDAVCLIENT_H

#include <QtCore/QByteArray>
#include <QtCore/QMap>
#include <QtCore/QObject>
#include <QtCore/QPair>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QUrl>

#include "vcard.h"

class QNetworkAccessManager;

/// One downloaded contact, with the card text kept for later patching.
class FetchedCard
{
public:
    QString href;
    QString etag;
    QString card;
    VCardContact contact;
};

/// Server state compared against what the last sync recorded.
class ServerDiff
{
public:
    QStringList changed;
    QStringList deleted;
    QMap<QString, QString> serverEtags;
};

enum UploadOutcome
{
    UploadCreated,
    UploadUpdated,
    /// The server's copy moved on; the caller must download before uploading again.
    UploadConflict,
    UploadFailed
};

class UploadResult
{
public:
    UploadResult() : outcome(UploadFailed) {}

    UploadOutcome outcome;
    QString uid;
    QString href;
    QString error;
};

class CardDavClient : public QObject
{
    Q_OBJECT

public:
    /// origin is for diagnostics only: pointing the client at a local server is how the exact
    /// request it emits can be inspected without sending anyone's password anywhere. Empty means
    /// Google.
    CardDavClient(const QString &gmailAddress, const QString &appPassword,
                  const QString &origin = QString(), QObject *parent = 0);
    ~CardDavClient();

    /// Installs the PEM trust anchors shipped with the app, on top of whatever the device
    /// already trusts. Safe to call more than once; does nothing when Qt has no SSL support.
    static bool installTrustBundle(const QByteArray &pem);

    /// True when this Qt build can do TLS at all. Everything else here fails without it.
    static bool sslAvailable();

    /// Every contact path and its etag. Returns false and sets lastError() on failure.
    bool list(QMap<QString, QString> *etags);

    /// The server listing against the etags the last sync stored.
    bool getChanges(const QMap<QString, QString> &knownEtags, ServerDiff *diff);

    /// The cards for a set of paths. Batches with addressbook-multiget (RFC 6352 section 8.7) and
    /// falls back to one GET per contact if the server refuses the REPORT. progressOffset is how
    /// many the caller already fetched in earlier calls, so the progress message counts from there
    /// ("Downloaded 370 of 400") rather than from zero for every batch.
    bool fetch(const QStringList &hrefs, const QMap<QString, QString> &serverEtags,
               QList<FetchedCard> *cards, int progressOffset = 0);

    /// PUTs one card. Pass an empty href to create the contact at a path derived from the UID.
    /// knownEtag is sent as If-Match, so a contact that changed on Google in the meantime comes
    /// back as a conflict instead of being overwritten from a stale copy.
    UploadResult upload(const QString &uid, const QString &href, const QString &knownEtag,
                        const QString &cardText);

    /// Where a contact with this UID would live on the server.
    QString hrefForNewUid(const QString &uid) const;

    /// The collection this client talks to, percent-encoded. Exposed for diagnostics and tests.
    QString collectionUrl() const { return m_origin + m_collectionPath; }

    /// Turns one of this class's already-encoded URL strings into a QUrl without encoding it a
    /// second time. Public only so a test can prove it: getting this wrong sent Google
    /// "someone%2540gmail.com" and earned a 404.
    static QUrl toRequestUrl(const QString &encodedUrl);

    /// A message meant to be shown to the user as it is.
    QString lastError() const { return m_lastError; }

    /// Asks the in-flight request to give up. Safe from a progress callback.
    void cancel() { m_cancelled = true; }
    bool wasCancelled() const { return m_cancelled; }

signals:
    /// Human-readable progress, for the UI.
    void progress(const QString &message);

private:
    class Response
    {
    public:
        Response() : status(0) {}
        int status;
        QByteArray body;
        QString etag;
        QString transportError;
    };

    Response send(const QByteArray &verb, const QString &url,
                  const QByteArray &body, const QByteArray &contentType,
                  const QList<QPair<QByteArray, QByteArray> > &headers);

    bool multiget(const QStringList &hrefs, QList<FetchedCard> *cards);
    bool getOne(const QString &href, FetchedCard *card);

    /// Turns a non-2xx status into lastError(), in words that say what to do about it.
    void describeFailure(const Response &r, const QString &what);

    QNetworkAccessManager *m_net;
    QString m_origin;
    QByteArray m_authorization;
    QString m_collectionPath;   // path only, no scheme or host
    QString m_lastError;
    bool m_multigetWorks;
    bool m_cancelled;
};

#endif // CARDDAVCLIENT_H
