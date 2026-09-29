// GContactsSync - a Google Contacts CardDAV client for Symbian^3 / Anna / Belle.
// Copyright (C) 2026 - GPL-3.0-or-later, see LICENSE.
#include "contacthash.h"
#include "vcard.h"

namespace {

void append(QString *out, const QString &value)
{
    *out += QLatin1Char('|');
    if (!value.isEmpty()) *out += value.trimmed().toLower();
}

/// FNV-1a, 64 bit. Not a cryptographic hash and does not need to be - the only question it
/// answers is "is this exactly what we recorded last time".
QString fnv64(const QString &text)
{
    quint64 hash = Q_UINT64_C(14695981039346656037);
    for (int i = 0; i < text.size(); ++i) {
        const ushort ch = text.at(i).unicode();
        hash ^= (quint8)(ch & 0xFF);
        hash *= Q_UINT64_C(1099511628211);
        hash ^= (quint8)(ch >> 8);
        hash *= Q_UINT64_C(1099511628211);
    }
    return QString::number(hash, 16).rightJustified(16, QLatin1Char('0'));
}

} // anonymous namespace

namespace ContactHash {

QString compute(const VCardContact &c)
{
    QString s;

    append(&s, c.firstName);
    append(&s, c.lastName);
    append(&s, c.middleName);
    append(&s, c.namePrefix);
    append(&s, c.nameSuffix);
    append(&s, c.nickname);
    append(&s, c.organization);
    append(&s, c.jobTitle);
    append(&s, c.notes);
    append(&s, c.birthday);

    for (int i = 0; i < c.phones.size(); ++i) {
        s += QLatin1String("|T:") + c.phones.at(i).type + QLatin1Char(':');
        // Formatting differences alone ("+1 800-555" against "+18005550") are not edits.
        s += VCardWriter::normalisePhone(c.phones.at(i).number);
    }
    for (int i = 0; i < c.emails.size(); ++i) {
        s += QLatin1String("|E:") + c.emails.at(i).type + QLatin1Char(':');
        s += c.emails.at(i).address.trimmed().toLower();
    }
    for (int i = 0; i < c.addresses.size(); ++i) {
        const ContactAddress &a = c.addresses.at(i);
        s += QLatin1String("|A:") + a.type + QLatin1Char(':');
        append(&s, a.street);
        append(&s, a.city);
        append(&s, a.region);
        append(&s, a.postalCode);
        append(&s, a.country);
    }
    for (int i = 0; i < c.websites.size(); ++i) {
        s += QLatin1String("|U:");
        append(&s, c.websites.at(i).url);
    }

    return fnv64(s);
}

} // namespace ContactHash
