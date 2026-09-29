// GContactsSync - a Google Contacts CardDAV client for Symbian^3 / Anna / Belle.
// Copyright (C) 2026 - GPL-3.0-or-later, see LICENSE.
//
// vCard 3.0 reading and writing. See vcard.h for why the writer patches rather than rebuilds.
#include "vcard.h"

#include <QtCore/QDate>

bool VCardContact::isEmpty() const
{
    return firstName.isEmpty() && lastName.isEmpty() && displayName.isEmpty()
        && organization.isEmpty() && phones.isEmpty() && emails.isEmpty();
}

// ============================================================================
// VCardText
// ============================================================================
namespace VCardText {

QStringList unfold(const QString &card)
{
    QStringList lines;
    if (card.isEmpty()) return lines;

    QString current;
    bool have = false;

    const QStringList raw = card.split(QLatin1Char('\n'));
    for (int i = 0; i < raw.size(); ++i) {
        QString line = raw.at(i);
        if (line.endsWith(QLatin1Char('\r'))) line.chop(1);

        if (!line.isEmpty() && (line.at(0) == QLatin1Char(' ') || line.at(0) == QLatin1Char('\t'))) {
            // Continuation: the folding whitespace is not part of the value.
            if (have) current += line.mid(1);
            continue;
        }

        if (have) lines.append(current);
        current = line;
        have = true;
    }
    if (have) lines.append(current);

    for (int i = lines.size() - 1; i >= 0; --i)
        if (lines.at(i).trimmed().isEmpty()) lines.removeAt(i);

    return lines;
}

QString propertyName(const QString &line)
{
    const int colon = line.indexOf(QLatin1Char(':'));
    if (colon <= 0) return QString();

    QString head = line.left(colon);
    const int semi = head.indexOf(QLatin1Char(';'));
    if (semi >= 0) head = head.left(semi);

    const int dot = head.indexOf(QLatin1Char('.'));
    if (dot >= 0) head = head.mid(dot + 1);

    return head.trimmed().toUpper();
}

QString groupName(const QString &line)
{
    const int colon = line.indexOf(QLatin1Char(':'));
    const QString head = colon > 0 ? line.left(colon) : line;
    const int dot = head.indexOf(QLatin1Char('.'));
    if (dot <= 0) return QString();

    const QString group = head.left(dot).trimmed();
    return group.startsWith(QLatin1String("item"), Qt::CaseInsensitive) ? group : QString();
}

QString rawValue(const QString &line)
{
    const int colon = line.indexOf(QLatin1Char(':'));
    return colon < 0 ? QString() : line.mid(colon + 1);
}

QString head(const QString &line)
{
    const int colon = line.indexOf(QLatin1Char(':'));
    return (colon > 0 ? line.left(colon) : line).toUpper();
}

QString unescape(const QString &s)
{
    if (s.isEmpty()) return s;

    QString out;
    out.reserve(s.size());
    for (int i = 0; i < s.size(); ++i) {
        if (s.at(i) != QLatin1Char('\\') || i + 1 >= s.size()) { out += s.at(i); continue; }

        const QChar next = s.at(++i);
        switch (next.unicode()) {
        case 'n': case 'N': out += QLatin1Char('\n'); break;
        case '\\':          out += QLatin1Char('\\'); break;
        case ',':           out += QLatin1Char(','); break;
        case ';':           out += QLatin1Char(';'); break;
        default:            out += next; break;
        }
    }
    return out;
}

QString escape(const QString &s)
{
    if (s.isEmpty()) return QString();

    QString out = s;
    out.replace(QLatin1String("\\"), QLatin1String("\\\\"));
    out.replace(QLatin1String(";"), QLatin1String("\\;"));
    out.replace(QLatin1String(","), QLatin1String("\\,"));
    out.remove(QLatin1Char('\r'));
    out.replace(QLatin1String("\n"), QLatin1String("\\n"));
    return out;
}

QStringList splitComponents(const QString &value)
{
    QStringList parts;
    QString current;
    for (int i = 0; i < value.size(); ++i) {
        if (value.at(i) == QLatin1Char('\\') && i + 1 < value.size()) {
            current += value.at(i);
            current += value.at(i + 1);
            ++i;
            continue;
        }
        if (value.at(i) == QLatin1Char(';')) { parts.append(current); current.clear(); continue; }
        current += value.at(i);
    }
    parts.append(current);
    return parts;
}

void appendFolded(QString *out, const QString &line)
{
    const int limit = 75;
    if (line.size() <= limit) { *out += line; *out += QLatin1String("\r\n"); return; }

    *out += line.left(limit);
    *out += QLatin1String("\r\n");

    int pos = limit;
    while (pos < line.size()) {
        const int take = qMin(limit - 1, line.size() - pos);
        *out += QLatin1Char(' ');
        *out += line.mid(pos, take);
        *out += QLatin1String("\r\n");
        pos += take;
    }
}

} // namespace VCardText

// ============================================================================
// VCardParser
// ============================================================================
namespace {

/// The value of one parameter from an already upper-cased property head. Handles both
/// "TYPE=WORK" and the vCard 2.1 shorthand "TEL;WORK;VOICE", which Google still emits for some
/// accounts. Several values come back comma-joined.
QString extractParam(const QString &head, const QString &name)
{
    QStringList found;
    const QStringList segments = head.split(QLatin1Char(';'));

    for (int i = 1; i < segments.size(); ++i) {   // segment 0 is the property name
        QString segment = segments.at(i).trimmed();
        if (segment.isEmpty()) continue;

        const int eq = segment.indexOf(QLatin1Char('='));
        if (eq < 0) {
            // Bare parameter - vCard 2.1 style type.
            if (name == QLatin1String("TYPE")) found.append(segment.remove(QLatin1Char('"')));
            continue;
        }
        if (segment.left(eq) == name)
            found.append(segment.mid(eq + 1).remove(QLatin1Char('"')).trimmed());
    }

    return found.join(QLatin1String(","));
}

/// Google sends TYPE values that mix a label with a medium: "CELL,VOICE", "HOME,VOICE",
/// "INTERNET,WORK", "PREF". Only the label part means anything here; when no component is a
/// label, the caller's default applies.
QString normaliseType(const QString &raw, const QString &fallback)
{
    if (raw.isEmpty()) return fallback;

    bool fax = false;
    QString label;

    const QStringList parts = raw.toLower().split(QLatin1Char(','));
    for (int i = 0; i < parts.size(); ++i) {
        const QString p = parts.at(i).trimmed();
        if (p == QLatin1String("home")) { if (label.isEmpty()) label = QLatin1String("home"); }
        else if (p == QLatin1String("work")) { if (label.isEmpty()) label = QLatin1String("work"); }
        else if (p == QLatin1String("cell") || p == QLatin1String("mobile")
                 || p == QLatin1String("x-mobile")) { if (label.isEmpty()) label = QLatin1String("mobile"); }
        else if (p == QLatin1String("pager")) { if (label.isEmpty()) label = QLatin1String("pager"); }
        else if (p == QLatin1String("other")) { if (label.isEmpty()) label = QLatin1String("other"); }
        else if (p == QLatin1String("fax") || p == QLatin1String("x-fax")) fax = true;
        // internet / voice / pref / msg / x-* say nothing about the label.
    }

    // A fax is a fax whatever else the line claims; home/work only says which kind.
    if (fax) return label == QLatin1String("work") ? QLatin1String("workfax") : QLatin1String("homefax");
    return label.isEmpty() ? fallback : label;
}

void parseName(const QString &value, VCardContact *c)
{
    // N:Family;Given;Additional;Prefix;Suffix
    const QStringList parts = VCardText::splitComponents(value);
    if (parts.size() > 0) c->lastName   = VCardText::unescape(parts.at(0)).trimmed();
    if (parts.size() > 1) c->firstName  = VCardText::unescape(parts.at(1)).trimmed();
    if (parts.size() > 2) c->middleName = VCardText::unescape(parts.at(2)).trimmed();
    if (parts.size() > 3) c->namePrefix = VCardText::unescape(parts.at(3)).trimmed();
    if (parts.size() > 4) c->nameSuffix = VCardText::unescape(parts.at(4)).trimmed();
}

ContactAddress parseAddress(const QString &head, const QString &value)
{
    // ADR:PostOfficeBox;Extended;Street;Locality;Region;PostalCode;Country
    const QStringList parts = VCardText::splitComponents(value);

    ContactAddress a;
    a.street     = parts.size() > 2 ? VCardText::unescape(parts.at(2)).trimmed() : QString();
    a.city       = parts.size() > 3 ? VCardText::unescape(parts.at(3)).trimmed() : QString();
    a.region     = parts.size() > 4 ? VCardText::unescape(parts.at(4)).trimmed() : QString();
    a.postalCode = parts.size() > 5 ? VCardText::unescape(parts.at(5)).trimmed() : QString();
    a.country    = parts.size() > 6 ? VCardText::unescape(parts.at(6)).trimmed() : QString();
    a.type       = normaliseType(extractParam(head, QLatin1String("TYPE")), QLatin1String("other"));
    return a;
}

} // anonymous namespace

namespace VCardParser {

QStringList splitCards(const QString &body)
{
    QStringList cards;
    if (body.isEmpty()) return cards;

    int from = 0;
    forever {
        const int begin = body.indexOf(QLatin1String("BEGIN:VCARD"), from, Qt::CaseInsensitive);
        if (begin < 0) break;

        int end = body.indexOf(QLatin1String("END:VCARD"), begin, Qt::CaseInsensitive);
        if (end < 0) break;

        end += 9;   // strlen("END:VCARD")
        cards.append(body.mid(begin, end - begin));
        from = end;
    }
    return cards;
}

VCardContact parseFirst(const QString &body, bool *ok)
{
    const QStringList cards = splitCards(body);
    if (cards.isEmpty()) {
        if (ok) *ok = false;
        return VCardContact();
    }
    if (ok) *ok = true;
    return parse(cards.first());
}

VCardContact parse(const QString &card)
{
    VCardContact c;
    const QStringList lines = VCardText::unfold(card);

    for (int i = 0; i < lines.size(); ++i) {
        const QString &line = lines.at(i);

        const QString prop = VCardText::propertyName(line);
        if (prop.isEmpty()) continue;

        // X-ABLabel carries the display name of a grouped field ("item1.X-ABLabel:_$!<Home>!$_").
        // It is Apple/Google plumbing, never contact data.
        if (prop.startsWith(QLatin1String("X-AB"))) continue;

        const QString head = VCardText::head(line);
        const QString value = VCardText::rawValue(line).trimmed();

        if (prop == QLatin1String("FN")) {
            c.displayName = VCardText::unescape(value);
        } else if (prop == QLatin1String("N")) {
            parseName(value, &c);
        } else if (prop == QLatin1String("EMAIL")) {
            const QString address = VCardText::unescape(value);
            if (!address.isEmpty())
                c.emails.append(ContactEmail(
                    address, normaliseType(extractParam(head, QLatin1String("TYPE")),
                                           QLatin1String("home"))));
        } else if (prop == QLatin1String("TEL")) {
            const QString number = VCardText::unescape(value);
            // "other" and not "mobile": a number Google carries with no label, or labelled Other,
            // is not a mobile and should not be filed as one. It becomes the phonebook's plain
            // telephone field, which is the platform's own unlabelled default.
            if (!number.isEmpty())
                c.phones.append(ContactPhone(
                    number, normaliseType(extractParam(head, QLatin1String("TYPE")),
                                          QLatin1String("other"))));
        } else if (prop == QLatin1String("ADR")) {
            const ContactAddress a = parseAddress(head, value);
            if (!a.isEmpty()) c.addresses.append(a);
        } else if (prop == QLatin1String("URL")) {
            const QString url = VCardText::unescape(value);
            if (!url.isEmpty())
                c.websites.append(ContactWebsite(
                    url, normaliseType(extractParam(head, QLatin1String("TYPE")),
                                       QLatin1String("other"))));
        } else if (prop == QLatin1String("ORG")) {
            // "Acme;Sales" - only the organisation name has anywhere to go on the phone.
            c.organization = VCardText::unescape(VCardText::splitComponents(value).value(0));
        } else if (prop == QLatin1String("TITLE")) {
            c.jobTitle = VCardText::unescape(value);
        } else if (prop == QLatin1String("NOTE")) {
            c.notes = VCardText::unescape(value);
        } else if (prop == QLatin1String("NICKNAME")) {
            c.nickname = VCardText::unescape(value);
        } else if (prop == QLatin1String("BDAY")) {
            c.birthday = value;
        } else if (prop == QLatin1String("UID")) {
            c.uid = value;
        } else if (prop == QLatin1String("PHOTO")) {
            // Inline base64 is what Google sends and what the phonebook can be given. A URI
            // form would need a second authenticated fetch, so it is left in the stored card
            // and otherwise ignored.
            if (head.contains(QLatin1String("ENCODING=B")) || head.contains(QLatin1String("BASE64"))) {
                c.photoBase64 = QString(value).remove(QLatin1Char(' '));
                c.photoType = extractParam(head, QLatin1String("TYPE"));
            }
        }
    }

    if (c.displayName.isEmpty())
        c.displayName = (c.firstName + QLatin1Char(' ') + c.lastName).trimmed();
    if (c.displayName.isEmpty())
        c.displayName = c.organization;

    return c;
}

} // namespace VCardParser

// ============================================================================
// VCardWriter
// ============================================================================
namespace {

// Properties this app owns: on a patch they are removed and re-emitted from the phone's state.
// Everything else in the stored card is passed through untouched.
const char * const kManaged[] = {
    "FN", "N", "NICKNAME", "ORG", "TITLE", "NOTE", "BDAY", "TEL", "EMAIL", "ADR", "URL", 0
};

// Never copied from the stored card: VERSION is re-emitted, and REV and PRODID are the server's
// to set, where a stale value is worse than none.
const char * const kDropped[] = { "BEGIN", "END", "VERSION", "REV", "PRODID", 0 };

bool isIn(const char * const *set, const QString &value)
{
    for (int i = 0; set[i]; ++i)
        if (value == QLatin1String(set[i])) return true;
    return false;
}

QString phoneTypeParam(const QString &type)
{
    if (type == QLatin1String("home")) return QLatin1String("HOME,VOICE");
    if (type == QLatin1String("work")) return QLatin1String("WORK,VOICE");
    if (type == QLatin1String("mobile")) return QLatin1String("CELL,VOICE");
    if (type == QLatin1String("pager")) return QLatin1String("PAGER");
    if (type == QLatin1String("homefax")) return QLatin1String("HOME,FAX");
    if (type == QLatin1String("workfax")) return QLatin1String("WORK,FAX");
    // An unlabelled number goes back without a TYPE at all, the way it almost certainly arrived,
    // rather than being given one it never had. Same treatment as an untyped URL.
    return QString();
}

QString emailTypeParam(const QString &type)
{
    if (type == QLatin1String("home")) return QLatin1String("INTERNET,HOME");
    if (type == QLatin1String("work")) return QLatin1String("INTERNET,WORK");
    return QLatin1String("INTERNET");
}

QString labelParam(const QString &type)
{
    if (type == QLatin1String("home")) return QLatin1String("HOME");
    if (type == QLatin1String("work")) return QLatin1String("WORK");
    return QLatin1String("OTHER");
}

/// A BDAY value Google accepts (yyyy-MM-dd), or a null string when there is nothing usable.
/// A contact with no birth year is stored as --MMdd and passes through unchanged.
QString normaliseBirthday(const QString &raw)
{
    if (raw.isEmpty()) return QString();

    const QString s = raw.trimmed();
    if (s.startsWith(QLatin1String("--"))) return s;

    const QString digits = QString(s).remove(QLatin1Char('-'));
    if (digits.size() < 8) return QString();

    bool okY = false, okM = false, okD = false;
    const int year = digits.mid(0, 4).toInt(&okY);
    const int month = digits.mid(4, 2).toInt(&okM);
    const int day = digits.mid(6, 2).toInt(&okD);
    if (!okY || !okM || !okD) return QString();
    if (month < 1 || month > 12 || day < 1 || day > 31) return QString();

    return QString::fromLatin1("%1-%2-%3")
        .arg(year, 4, 10, QLatin1Char('0'))
        .arg(month, 2, 10, QLatin1Char('0'))
        .arg(day, 2, 10, QLatin1Char('0'));
}

bool orgMatches(const QString &orgLine, const VCardContact &phone)
{
    if (phone.organization.isEmpty()) return false;

    const QStringList parts = VCardText::splitComponents(VCardText::rawValue(orgLine));
    if (parts.size() < 2) return false;   // no department to protect

    return VCardText::unescape(parts.at(0)).trimmed() == phone.organization.trimmed();
}

/// The properties this app owns, as vCard lines.
QStringList managedLines(const VCardContact &c)
{
    QStringList lines;

    QString displayName = c.displayName;
    if (displayName.isEmpty())
        displayName = (c.firstName + QLatin1Char(' ') + c.lastName).trimmed();
    if (displayName.isEmpty()) displayName = c.organization;
    if (!displayName.isEmpty())
        lines.append(QLatin1String("FN:") + VCardText::escape(displayName));

    // N is mandatory in vCard 3.0, so it goes out even when every component is empty.
    lines.append(QLatin1String("N:") + VCardText::escape(c.lastName) + QLatin1Char(';')
                 + VCardText::escape(c.firstName) + QLatin1Char(';')
                 + VCardText::escape(c.middleName) + QLatin1Char(';')
                 + VCardText::escape(c.namePrefix) + QLatin1Char(';')
                 + VCardText::escape(c.nameSuffix));

    if (!c.nickname.isEmpty())
        lines.append(QLatin1String("NICKNAME:") + VCardText::escape(c.nickname));

    for (int i = 0; i < c.phones.size(); ++i) {
        const QString line = VCardWriter::phoneLine(c.phones.at(i));
        if (!line.isEmpty()) lines.append(line);
    }
    for (int i = 0; i < c.emails.size(); ++i) {
        const QString line = VCardWriter::emailLine(c.emails.at(i));
        if (!line.isEmpty()) lines.append(line);
    }
    for (int i = 0; i < c.addresses.size(); ++i) {
        const QString line = VCardWriter::addressLine(c.addresses.at(i));
        if (!line.isEmpty()) lines.append(line);
    }
    for (int i = 0; i < c.websites.size(); ++i) {
        const QString line = VCardWriter::websiteLine(c.websites.at(i));
        if (!line.isEmpty()) lines.append(line);
    }

    if (!c.organization.isEmpty())
        lines.append(QLatin1String("ORG:") + VCardText::escape(c.organization));
    if (!c.jobTitle.isEmpty())
        lines.append(QLatin1String("TITLE:") + VCardText::escape(c.jobTitle));
    if (!c.notes.isEmpty())
        lines.append(QLatin1String("NOTE:") + VCardText::escape(c.notes));

    const QString bday = normaliseBirthday(c.birthday);
    if (!bday.isEmpty()) lines.append(QLatin1String("BDAY:") + bday);

    return lines;
}

} // anonymous namespace

namespace VCardWriter {

QString phoneLine(const ContactPhone &p)
{
    if (p.number.isEmpty()) return QString();

    const QString param = phoneTypeParam(p.type);
    return param.isEmpty()
        ? QLatin1String("TEL:") + VCardText::escape(p.number)
        : QLatin1String("TEL;TYPE=") + param + QLatin1Char(':') + VCardText::escape(p.number);
}

QString emailLine(const ContactEmail &e)
{
    if (e.address.isEmpty()) return QString();
    return QLatin1String("EMAIL;TYPE=") + emailTypeParam(e.type) + QLatin1Char(':')
         + VCardText::escape(e.address);
}

QString addressLine(const ContactAddress &a)
{
    if (a.isEmpty()) return QString();
    return QLatin1String("ADR;TYPE=") + labelParam(a.type) + QLatin1String(":;;")
         + VCardText::escape(a.street) + QLatin1Char(';')
         + VCardText::escape(a.city) + QLatin1Char(';')
         + VCardText::escape(a.region) + QLatin1Char(';')
         + VCardText::escape(a.postalCode) + QLatin1Char(';')
         + VCardText::escape(a.country);
}

QString websiteLine(const ContactWebsite &w)
{
    if (w.url.isEmpty()) return QString();

    const QString label = labelParam(w.type);
    return label == QLatin1String("OTHER")
        ? QLatin1String("URL:") + VCardText::escape(w.url)
        : QLatin1String("URL;TYPE=") + label + QLatin1Char(':') + VCardText::escape(w.url);
}

QString buildNew(const VCardContact &c, const QString &uid)
{
    QString out;
    out += QLatin1String("BEGIN:VCARD\r\n");
    out += QLatin1String("VERSION:3.0\r\n");
    VCardText::appendFolded(&out, QLatin1String("UID:") + uid);

    const QStringList lines = managedLines(c);
    for (int i = 0; i < lines.size(); ++i)
        VCardText::appendFolded(&out, lines.at(i));

    out += QLatin1String("END:VCARD\r\n");
    return out;
}

QString patch(const QString &storedCard, const VCardContact &phone)
{
    const QStringList original = VCardText::unfold(storedCard);

    QStringList passthrough;
    bool orgPassedThrough = false;

    for (int i = 0; i < original.size(); ++i) {
        const QString &line = original.at(i);

        const QString prop = VCardText::propertyName(line);
        if (prop.isEmpty()) continue;
        if (isIn(kDropped, prop)) continue;

        if (prop.startsWith(QLatin1String("X-AB"))) {
            // A grouped X-ABLabel is the label of one value ("item1.EMAIL"); that value is about
            // to be re-emitted without the group, so the label goes with it. An ungrouped X-AB
            // property (X-ABShowAs, say) is contact data of its own.
            if (VCardText::groupName(line).isEmpty()) passthrough.append(line);
            continue;
        }

        if (isIn(kManaged, prop)) {
            // ORG can carry a department ("Acme;Sales") that the phone cannot hold. When the
            // company name still matches, keep the original line and do not re-emit.
            if (prop == QLatin1String("ORG") && orgMatches(line, phone)) {
                passthrough.append(line);
                orgPassedThrough = true;
            }
            continue;
        }

        passthrough.append(line);
    }

    QString out;
    out += QLatin1String("BEGIN:VCARD\r\n");
    out += QLatin1String("VERSION:3.0\r\n");

    const QStringList lines = managedLines(phone);
    for (int i = 0; i < lines.size(); ++i) {
        if (orgPassedThrough && VCardText::propertyName(lines.at(i)) == QLatin1String("ORG"))
            continue;
        VCardText::appendFolded(&out, lines.at(i));
    }
    for (int i = 0; i < passthrough.size(); ++i)
        VCardText::appendFolded(&out, passthrough.at(i));

    out += QLatin1String("END:VCARD\r\n");
    return out;
}

QString normalisePhone(const QString &s)
{
    if (s.isEmpty()) return QString();

    QString out;
    out.reserve(s.size());
    for (int i = 0; i < s.size(); ++i) {
        const QChar ch = s.at(i);
        if (ch.isDigit() || (ch == QLatin1Char('+') && out.isEmpty())) out += ch;
    }
    return out;
}

} // namespace VCardWriter
