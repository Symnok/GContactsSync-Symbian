// GContactsSync - a Google Contacts CardDAV client for Symbian^3 / Anna / Belle.
// Copyright (C) 2026 - GPL-3.0-or-later, see LICENSE.
#include "carddavclient.h"

#include <QtCore/QBuffer>
#include <QtCore/QEventLoop>
#include <QtCore/QTimer>
#include <QtCore/QUrl>
#include <QtCore/QXmlStreamReader>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>

#ifndef QT_NO_OPENSSL
#include <QtNetwork/QSslCertificate>
#include <QtNetwork/QSslConfiguration>
#include <QtNetwork/QSslSocket>
#endif

namespace {

const char * const kOrigin = "https://www.google.com";
const char * const kDavNs = "DAV:";
const char * const kCardDavNs = "urn:ietf:params:xml:ns:carddav";

/// How many contacts to ask for in one multiget. Google accepts far more, but the whole response
/// is buffered, and cards with photos run to tens of kilobytes each - this runs on a phone with a
/// few megabytes of heap.
const int kMultigetBatch = 20;

/// Per-request ceiling. A multiget of twenty cards with photos over GPRS is not quick.
const int kTimeoutMs = 90000;

QString xmlEscape(const QString &s)
{
    QString out = s;
    out.replace(QLatin1String("&"), QLatin1String("&amp;"));
    out.replace(QLatin1String("<"), QLatin1String("&lt;"));
    out.replace(QLatin1String(">"), QLatin1String("&gt;"));
    return out;
}

class MultistatusEntry
{
public:
    QString href;
    QString etag;
    QString card;
};

/// The href / getetag / address-data of every response in a DAV multistatus body.
///
/// A real XML parser rather than string scanning: a multiget response carries whole vCards as
/// escaped element text, and getting the entity decoding wrong corrupts contacts.
bool readMultistatus(const QByteArray &xml, QList<MultistatusEntry> *out, QString *error)
{
    QXmlStreamReader reader(xml);

    MultistatusEntry current;
    bool inResponse = false;
    // A propstat can report a property as "404 not found"; only the 200 ones carry values.
    bool propstatOk = true;

    while (!reader.atEnd()) {
        reader.readNext();
        if (reader.hasError()) break;

        if (reader.isStartElement()) {
            const QString ns = reader.namespaceUri().toString();
            const QString name = reader.name().toString();

            if (ns == QLatin1String(kDavNs) && name == QLatin1String("response")) {
                current = MultistatusEntry();
                inResponse = true;
                propstatOk = true;
            } else if (!inResponse) {
                continue;
            } else if (ns == QLatin1String(kDavNs) && name == QLatin1String("propstat")) {
                propstatOk = true;
            } else if (ns == QLatin1String(kDavNs) && name == QLatin1String("status")) {
                propstatOk = reader.readElementText().contains(QLatin1String(" 200 "));
            } else if (ns == QLatin1String(kDavNs) && name == QLatin1String("href")) {
                current.href = reader.readElementText().trimmed();
            } else if (propstatOk && ns == QLatin1String(kDavNs)
                       && name == QLatin1String("getetag")) {
                current.etag = reader.readElementText().trimmed().remove(QLatin1Char('"'));
            } else if (propstatOk && ns == QLatin1String(kCardDavNs)
                       && name == QLatin1String("address-data")) {
                current.card = reader.readElementText();
            }
        } else if (reader.isEndElement()
                   && reader.namespaceUri().toString() == QLatin1String(kDavNs)
                   && reader.name() == QLatin1String("response")) {
            out->append(current);
            inResponse = false;
        }
    }

    if (reader.hasError()) {
        *error = QObject::tr("Google's reply could not be understood (%1).")
                     .arg(reader.errorString());
        return false;
    }
    return true;
}

QString firstCardText(const QString &body)
{
    const QStringList cards = VCardParser::splitCards(body);
    return cards.isEmpty() ? body : cards.first();
}

} // anonymous namespace

// ============================================================================
// Construction and TLS
// ============================================================================
CardDavClient::CardDavClient(const QString &gmailAddress, const QString &appPassword,
                             const QString &origin, QObject *parent)
    : QObject(parent)
    , m_net(new QNetworkAccessManager(this))
    , m_origin(origin.isEmpty() ? QLatin1String(kOrigin) : origin)
    , m_multigetWorks(true)
    , m_cancelled(false)
{
    // Percent-encoded here, and it must stay that way: send() builds the request URL with
    // QUrl::fromEncoded for exactly this reason.
    m_collectionPath = QLatin1String("/carddav/v1/principals/")
                     + QString::fromLatin1(QUrl::toPercentEncoding(gmailAddress))
                     + QLatin1String("/lists/default/");

    const QByteArray raw = (gmailAddress + QLatin1Char(':') + appPassword).toUtf8();
    m_authorization = "Basic " + raw.toBase64();
}

CardDavClient::~CardDavClient()
{
}

bool CardDavClient::sslAvailable()
{
#ifdef QT_NO_OPENSSL
    return false;
#else
    return QSslSocket::supportsSsl();
#endif
}

bool CardDavClient::installTrustBundle(const QByteArray &pem)
{
#ifdef QT_NO_OPENSSL
    Q_UNUSED(pem)
    return false;
#else
    if (pem.isEmpty()) return false;

    const QList<QSslCertificate> extra = QSslCertificate::fromData(pem, QSsl::Pem);
    if (extra.isEmpty()) return false;

    // Added to what the device already trusts rather than replacing it: the point is only to
    // supply roots a 2011 store never had, not to distrust anything it does have.
    QSslConfiguration config = QSslConfiguration::defaultConfiguration();
    QList<QSslCertificate> all = config.caCertificates();
    all += extra;
    config.setCaCertificates(all);
    QSslConfiguration::setDefaultConfiguration(config);
    return true;
#endif
}

// fromEncoded, NOT QUrl(QString). Every URL this class builds is assembled from percent-encoded
// pieces - the address in the collection path, and the hrefs Google hands back - and QUrl(QString)
// treats its argument as UNencoded, so it escapes the '%' of an existing escape all over again.
// That turned "someone%40gmail.com" into "someone%2540gmail.com": Google has no principal by that
// name, so it answered 404 and the app reported the address book as missing.
QUrl CardDavClient::toRequestUrl(const QString &encodedUrl)
{
    return QUrl::fromEncoded(encodedUrl.toLatin1());
}

QString CardDavClient::hrefForNewUid(const QString &uid) const
{
    return m_collectionPath + QString::fromLatin1(QUrl::toPercentEncoding(uid))
         + QLatin1String(".vcf");
}

// ============================================================================
// One request, waited for
// ============================================================================
CardDavClient::Response CardDavClient::send(
    const QByteArray &verb, const QString &url,
    const QByteArray &body, const QByteArray &contentType,
    const QList<QPair<QByteArray, QByteArray> > &headers)
{
    Response result;

    QNetworkRequest request;
    request.setUrl(toRequestUrl(url));
    request.setRawHeader("Authorization", m_authorization);
    request.setRawHeader("User-Agent", "GContactsSync/1.0");
    // No gzip: shipping a decompressor buys little here, and identity responses remove a whole
    // class of failure on a platform that is awkward to debug.
    request.setRawHeader("Accept-Encoding", "identity");
    if (!contentType.isEmpty()) request.setRawHeader("Content-Type", contentType);
    for (int i = 0; i < headers.size(); ++i)
        request.setRawHeader(headers.at(i).first, headers.at(i).second);

    QBuffer buffer;
    QNetworkReply *reply = 0;

    if (verb == "GET") {
        reply = m_net->get(request);
    } else if (verb == "PUT") {
        reply = m_net->put(request, body);
    } else {
        // PROPFIND and REPORT. sendCustomRequest wants a QIODevice, which has to outlive the
        // request - hence the buffer on the stack rather than a temporary.
        buffer.setData(body);
        buffer.open(QIODevice::ReadOnly);
        reply = m_net->sendCustomRequest(request, verb, &buffer);
    }

    // Wait. The timer is what stops a stalled GPRS connection hanging the app for ever: QNetwork
    // has no request timeout of its own in Qt 4.
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    connect(reply, SIGNAL(finished()), &loop, SLOT(quit()));
    connect(&timeout, SIGNAL(timeout()), &loop, SLOT(quit()));
    timeout.start(kTimeoutMs);
    loop.exec(QEventLoop::ExcludeUserInputEvents);

    if (!reply->isFinished()) {
        reply->abort();
        result.transportError = tr("the request timed out");
    } else if (reply->error() != QNetworkReply::NoError) {
        // An HTTP error status is not a transport failure: the body and the code still matter, and
        // the callers turn 401/403/412 into something useful.
        const QVariant code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
        if (!code.isValid()) result.transportError = reply->errorString();
    }

    result.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    result.body = reply->readAll();
    result.etag = QString::fromLatin1(reply->rawHeader("ETag")).remove(QLatin1Char('"'));

    reply->deleteLater();
    return result;
}

void CardDavClient::describeFailure(const Response &r, const QString &what)
{
    if (!r.transportError.isEmpty()) {
        m_lastError = tr("Could not reach Google: %1.").arg(r.transportError);
        return;
    }

    switch (r.status) {
    case 401:
        m_lastError = tr("Google rejected the sign-in (401).\n\n"
                         "Use a 16-character Google App Password, not your account password. "
                         "Create one at myaccount.google.com/apppasswords - it needs 2-Step "
                         "Verification switched on.");
        return;
    case 403:
        m_lastError = tr("Google refused access (403).\n\n"
                         "Check that the account allows CardDAV and that the App Password has not "
                         "been revoked.");
        return;
    case 404:
        // Naming the path matters: a 404 here means Google has no principal by that
        // name, and the path is the only thing that shows why - a mistyped address looks
        // exactly like a URL this app mangled on the way out.
        m_lastError = tr("Google has no address book at that address (404).\n\n"
                         "Asked for: %1\n\n"
                         "Check the Gmail address is spelt correctly and matches the "
                         "account the App Password belongs to.").arg(m_collectionPath);
        return;
    default:
        m_lastError = tr("Google returned %1 while %2.").arg(r.status).arg(what);
        return;
    }
}

// ============================================================================
// LIST
// ============================================================================
bool CardDavClient::list(QMap<QString, QString> *etags)
{
    static const char * const body =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<d:propfind xmlns:d=\"DAV:\">"
          "<d:prop><d:getetag/><d:resourcetype/></d:prop>"
        "</d:propfind>";

    QList<QPair<QByteArray, QByteArray> > headers;
    headers.append(qMakePair(QByteArray("Depth"), QByteArray("1")));

    const Response r = send("PROPFIND", m_origin + m_collectionPath,
                            QByteArray(body), "application/xml", headers);

    // PROPFIND answers 207 Multi-Status, which is a 2xx like any other.
    if (r.status < 200 || r.status > 299) {
        describeFailure(r, tr("listing contacts"));
        return false;
    }

    QList<MultistatusEntry> entries;
    if (!readMultistatus(r.body, &entries, &m_lastError)) return false;

    etags->clear();
    for (int i = 0; i < entries.size(); ++i) {
        const MultistatusEntry &e = entries.at(i);
        if (e.href.isEmpty() || e.href.endsWith(QLatin1Char('/'))) continue;   // the collection
        if (e.etag.isEmpty()) continue;
        etags->insert(e.href, e.etag);
    }
    return true;
}

bool CardDavClient::getChanges(const QMap<QString, QString> &knownEtags, ServerDiff *diff)
{
    QMap<QString, QString> serverEtags;
    if (!list(&serverEtags)) return false;

    diff->serverEtags = serverEtags;
    diff->changed.clear();
    diff->deleted.clear();

    for (QMap<QString, QString>::const_iterator it = serverEtags.constBegin();
         it != serverEtags.constEnd(); ++it) {
        const QString known = knownEtags.value(it.key());
        if (known.isEmpty() || known != it.value()) diff->changed.append(it.key());
    }
    for (QMap<QString, QString>::const_iterator it = knownEtags.constBegin();
         it != knownEtags.constEnd(); ++it) {
        if (!serverEtags.contains(it.key())) diff->deleted.append(it.key());
    }
    return true;
}

// ============================================================================
// FETCH
// ============================================================================
bool CardDavClient::fetch(const QStringList &hrefs, const QMap<QString, QString> &serverEtags,
                          QList<FetchedCard> *cards)
{
    cards->clear();
    if (hrefs.isEmpty()) return true;

    for (int offset = 0; offset < hrefs.size(); offset += kMultigetBatch) {
        if (m_cancelled) { m_lastError = tr("Sync cancelled."); return false; }

        const int count = qMin(kMultigetBatch, hrefs.size() - offset);
        const QStringList batch = hrefs.mid(offset, count);

        QList<FetchedCard> fetched;
        bool haveBatch = false;

        if (m_multigetWorks) {
            if (multiget(batch, &fetched) && !fetched.isEmpty()) {
                haveBatch = true;
            } else {
                // Accepted but useless, or refused: stop paying for it and use plain GETs.
                m_multigetWorks = false;
                fetched.clear();
            }
        }

        if (!haveBatch) {
            for (int i = 0; i < batch.size(); ++i) {
                if (m_cancelled) { m_lastError = tr("Sync cancelled."); return false; }
                FetchedCard card;
                if (getOne(batch.at(i), &card)) fetched.append(card);
            }
        }

        for (int i = 0; i < fetched.size(); ++i) {
            FetchedCard card = fetched.at(i);
            if (card.etag.isEmpty()) card.etag = serverEtags.value(card.href);
            card.contact.etag = card.etag;
            cards->append(card);
        }

        emit progress(tr("Downloaded %1 of %2...")
                          .arg(qMin(offset + count, hrefs.size())).arg(hrefs.size()));
    }
    return true;
}

bool CardDavClient::multiget(const QStringList &hrefs, QList<FetchedCard> *cards)
{
    QString body;
    body += QLatin1String("<?xml version=\"1.0\" encoding=\"UTF-8\"?>");
    body += QLatin1String("<C:addressbook-multiget xmlns:D=\"DAV:\" "
                          "xmlns:C=\"urn:ietf:params:xml:ns:carddav\">");
    body += QLatin1String("<D:prop><D:getetag/><C:address-data/></D:prop>");
    for (int i = 0; i < hrefs.size(); ++i)
        body += QLatin1String("<D:href>") + xmlEscape(hrefs.at(i)) + QLatin1String("</D:href>");
    body += QLatin1String("</C:addressbook-multiget>");

    QList<QPair<QByteArray, QByteArray> > headers;
    headers.append(qMakePair(QByteArray("Depth"), QByteArray("0")));   // RFC 6352 section 8.7

    const Response r = send("REPORT", m_origin + m_collectionPath,
                            body.toUtf8(), "application/xml", headers);
    if (r.status < 200 || r.status > 299) return false;

    QList<MultistatusEntry> entries;
    QString ignored;
    if (!readMultistatus(r.body, &entries, &ignored)) return false;

    for (int i = 0; i < entries.size(); ++i) {
        const MultistatusEntry &e = entries.at(i);
        if (e.card.isEmpty()) continue;

        bool ok = false;
        const VCardContact contact = VCardParser::parseFirst(e.card, &ok);
        if (!ok) continue;

        FetchedCard card;
        card.href = e.href;
        card.etag = e.etag;
        card.card = firstCardText(e.card);
        card.contact = contact;
        card.contact.href = e.href;
        cards->append(card);
    }
    return true;
}

bool CardDavClient::getOne(const QString &href, FetchedCard *card)
{
    const Response r = send("GET", m_origin + href,
                            QByteArray(), QByteArray(),
                            QList<QPair<QByteArray, QByteArray> >());
    if (r.status < 200 || r.status > 299) return false;

    const QString text = QString::fromUtf8(r.body);

    bool ok = false;
    const VCardContact contact = VCardParser::parseFirst(text, &ok);
    if (!ok) return false;

    card->href = href;
    card->etag = r.etag;
    card->card = firstCardText(text);
    card->contact = contact;
    card->contact.href = href;
    return true;
}

// ============================================================================
// UPLOAD
// ============================================================================
UploadResult CardDavClient::upload(const QString &uid, const QString &href,
                                   const QString &knownEtag, const QString &cardText)
{
    UploadResult result;
    result.uid = uid;

    const bool creating = href.isEmpty();
    result.href = creating ? hrefForNewUid(uid) : href;

    QList<QPair<QByteArray, QByteArray> > headers;
    if (creating)
        headers.append(qMakePair(QByteArray("If-None-Match"), QByteArray("*")));
    else if (!knownEtag.isEmpty())
        headers.append(qMakePair(QByteArray("If-Match"),
                                 '"' + knownEtag.toLatin1() + '"'));

    const Response r = send("PUT", m_origin + result.href,
                            cardText.toUtf8(), "text/vcard; charset=utf-8", headers);

    if (!r.transportError.isEmpty()) {
        result.outcome = UploadFailed;
        result.error = r.transportError;
        return result;
    }
    if (r.status == 412 || r.status == 409) {
        result.outcome = UploadConflict;
        result.error = tr("changed on Google since the last download");
        return result;
    }
    if (r.status < 200 || r.status > 299) {
        result.outcome = UploadFailed;
        result.error = tr("Google returned %1").arg(r.status);
        return result;
    }

    result.outcome = creating ? UploadCreated : UploadUpdated;
    return result;
}
