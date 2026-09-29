// GContactsSync - a Google Contacts CardDAV client for Symbian^3 / Anna / Belle.
// Copyright (C) 2026 - GPL-3.0-or-later, see LICENSE.
//
// One contact, in the only shape this app keeps in memory, plus the vCard 3.0 reading and
// writing around it. Both directions go through VCardContact:
//
//     Google  ->  vCard text  ->  VCardContact  ->  QContact (the phonebook)
//     phone   ->  QContact    ->  VCardContact  ->  vCard text -> Google
//
// Pure QtCore: no QtContacts, no Symbian, so the desktop harness in tools/ can exercise all of
// it. Ported from the Windows Phone 8.1 version of this app, where the same logic is covered by
// a test suite; tools/gcontactssync-cli carries that suite over.
#ifndef VCARD_H
#define VCARD_H

#include <QtCore/QList>
#include <QtCore/QString>
#include <QtCore/QStringList>

struct ContactPhone
{
    QString number;
    /// Normalised label: mobile / home / work / homefax / workfax / pager / other.
    QString type;

    ContactPhone() : type(QLatin1String("mobile")) {}
    ContactPhone(const QString &n, const QString &t) : number(n), type(t) {}
};

struct ContactEmail
{
    QString address;
    /// Normalised label: home / work / other.
    QString type;

    ContactEmail() : type(QLatin1String("home")) {}
    ContactEmail(const QString &a, const QString &t) : address(a), type(t) {}
};

struct ContactAddress
{
    QString street;
    QString city;
    QString region;
    QString postalCode;
    QString country;
    /// Normalised label: home / work / other.
    QString type;

    ContactAddress() : type(QLatin1String("other")) {}

    bool isEmpty() const
    {
        return street.isEmpty() && city.isEmpty() && region.isEmpty()
            && postalCode.isEmpty() && country.isEmpty();
    }
};

struct ContactWebsite
{
    QString url;
    QString type;

    ContactWebsite() : type(QLatin1String("other")) {}
    ContactWebsite(const QString &u, const QString &t) : url(u), type(t) {}
};

class VCardContact
{
public:
    QString displayName;
    QString firstName;
    QString lastName;
    QString middleName;
    QString namePrefix;
    QString nameSuffix;

    QList<ContactPhone> phones;
    QList<ContactEmail> emails;
    QList<ContactAddress> addresses;
    QList<ContactWebsite> websites;

    QString organization;
    QString jobTitle;
    QString notes;
    QString nickname;
    /// As sent: yyyy-MM-dd, yyyyMMdd, or --MMdd for a birthday with no year.
    QString birthday;

    /// Inline photo, base64 exactly as the vCard carried it (PHOTO;ENCODING=b).
    QString photoBase64;
    /// JPEG / PNG / GIF, from the PHOTO line's TYPE parameter.
    QString photoType;

    /// Google's UID. Also what the phone contact is tagged with, which is how the two are tied.
    QString uid;
    QString etag;
    /// CardDAV path, e.g. /carddav/v1/principals/me@gmail.com/lists/default/<uid>.vcf
    QString href;

    /// The phonebook's own id, only set for contacts read back off the phone.
    QString localId;

    bool isEmpty() const;
};

/// The line-level details of vCard text, in one place because the parser, the writer and the
/// patcher must agree on them exactly or a round trip corrupts cards.
namespace VCardText
{
/// Splits a card into logical lines, undoing RFC 2425 folding. Google folds long values -
/// photos in particular - so nothing works without this.
QStringList unfold(const QString &card);

/// The property name of a logical line, upper-cased, with Google's "itemN." grouping prefix and
/// any parameters removed. "item2.TEL;TYPE=WORK:+1" gives "TEL".
QString propertyName(const QString &line);

/// Google's "itemN" group of a line, or an empty string when it has none.
QString groupName(const QString &line);

/// Everything after the first colon, untouched.
QString rawValue(const QString &line);

/// The property and parameter section of a line, upper-cased.
QString head(const QString &line);

QString unescape(const QString &s);
QString escape(const QString &s);

/// Splits a structured value (N, ADR, ORG) on unescaped semicolons only.
QStringList splitComponents(const QString &value);

/// Appends a line folded at 75 octets, as RFC 2426 asks.
void appendFolded(QString *out, const QString &line);
}

namespace VCardParser
{
/// Every BEGIN:VCARD..END:VCARD block in a body, in order.
QStringList splitCards(const QString &body);

/// The first card in a body. Sets *ok to false when there is none.
VCardContact parseFirst(const QString &body, bool *ok = 0);

VCardContact parse(const QString &card);
}

namespace VCardWriter
{
/// A fresh card, for a contact that exists only on the phone.
QString buildNew(const VCardContact &c, const QString &uid);

/// The card Google last sent, with this app's properties brought up to date and every other
/// line left exactly as it came.
///
/// A CardDAV PUT replaces the whole card, so writing a fresh one from what the phone holds
/// would silently delete the contact photo, CATEGORIES, IMPP handles and every X- property
/// Google keeps. So the stored card is patched instead. (The Windows Phone version also had to
/// restore values its contact store had no room for; a Symbian QContact has no such limit, so
/// that part is gone.)
QString patch(const QString &storedCard, const VCardContact &phone);

/// Digits and a leading plus only, so "+1 (800) 555-0100" and "+18005550100" compare equal.
QString normalisePhone(const QString &s);

// Single-property line builders, exposed for tests.
QString phoneLine(const ContactPhone &p);
QString emailLine(const ContactEmail &e);
QString addressLine(const ContactAddress &a);
QString websiteLine(const ContactWebsite &w);
}

#endif // VCARD_H
