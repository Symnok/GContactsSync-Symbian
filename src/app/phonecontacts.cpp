// GContactsSync - a Google Contacts CardDAV client for Symbian^3 / Anna / Belle.
// Copyright (C) 2026 - GPL-3.0-or-later, see LICENSE.
//
// See phonecontacts.h for why this is native Symbian C++ and not Qt Mobility.
#include "phonecontacts.h"

#include <QtCore/QDate>
#include <QtCore/QFile>
#include <QtCore/QStringList>
#include <QtCore/QTextStream>

#include <cntdb.h>
#include <cntdef.h>
#include <cntfield.h>
#include <cntfldst.h>
#include <cntitem.h>

namespace {

/// The marker that says a contact is ours, on the first line of its note. Stripped on the way
/// back out, so it never reaches Google and never counts as an edit.
const char * const kTagPrefix = "[GID:";

/// A read-only descriptor over a QString. The QString must outlive it, so callers keep the value
/// in a named local rather than passing a temporary.
inline TPtrC16 toPtr(const QString &s)
{
    return TPtrC16(reinterpret_cast<const TUint16 *>(s.utf16()), s.length());
}

QString fromDes(const TDesC &d)
{
    return QString::fromUtf16(reinterpret_cast<const ushort *>(d.Ptr()), d.Length());
}

/// Adds one text field, labelled the way the native Contacts application labels them: a content
/// type, the vCard property it maps to, and optionally a vCard parameter (HOME / WORK / CELL...)
/// that becomes the visible label.
void addTextFieldL(CContactItem &item, TUid contentType, TUid vcardMap, TUid label,
                   const QString &value)
{
    if (value.isEmpty()) return;

    CContactItemField *field = CContactItemField::NewLC(KStorageTypeText, contentType);
    field->SetMapping(vcardMap);
    if (label.iUid != 0) field->AddFieldTypeL(label);
    field->TextStorage()->SetTextL(toPtr(value));

    // AddFieldL takes ownership, so the field is popped rather than destroyed.
    item.AddFieldL(*field);
    CleanupStack::Pop(field);
}

void addTextFieldL(CContactItem &item, TUid contentType, TUid vcardMap, const QString &value)
{
    addTextFieldL(item, contentType, vcardMap, TUid::Uid(0), value);
}

/// The vCard parameter that labels a phone number, from our normalised type.
TUid phoneLabelFor(const QString &type)
{
    if (type == QLatin1String("mobile")) return KUidContactFieldVCardMapCELL;
    if (type == QLatin1String("home")) return KUidContactFieldVCardMapHOME;
    if (type == QLatin1String("work")) return KUidContactFieldVCardMapWORK;
    if (type == QLatin1String("pager")) return KUidContactFieldVCardMapPAGER;
    if (type == QLatin1String("homefax")) return KUidContactFieldVCardMapHOME;
    if (type == QLatin1String("workfax")) return KUidContactFieldVCardMapWORK;
    return TUid::Uid(0);
}

/// The inverse. It must be the exact inverse of the two writers, or every download would look like
/// an edit and the next upload would push the whole address book back to Google.
QString phoneTypeFrom(const CContactItemField &field)
{
    const CContentType &type = field.ContentType();
    const bool fax = type.ContainsFieldType(KUidContactFieldVCardMapFAX);
    const bool home = type.ContainsFieldType(KUidContactFieldVCardMapHOME);
    const bool work = type.ContainsFieldType(KUidContactFieldVCardMapWORK);

    if (fax) return work ? QLatin1String("workfax") : QLatin1String("homefax");
    if (type.ContainsFieldType(KUidContactFieldVCardMapPAGER)) return QLatin1String("pager");
    if (type.ContainsFieldType(KUidContactFieldVCardMapCELL)) return QLatin1String("mobile");
    if (home) return QLatin1String("home");
    if (work) return QLatin1String("work");
    return QLatin1String("other");
}

QString labelTypeFrom(const CContactItemField &field)
{
    const CContentType &type = field.ContentType();
    if (type.ContainsFieldType(KUidContactFieldVCardMapHOME)) return QLatin1String("home");
    if (type.ContainsFieldType(KUidContactFieldVCardMapWORK)) return QLatin1String("work");
    return QLatin1String("other");
}

/// Every content type this app writes. A rewrite clears exactly these and nothing else: any other
/// field on the contact belongs to the user or to another application.
bool isManagedField(TUid contentType)
{
    return contentType == KUidContactFieldGivenName
        || contentType == KUidContactFieldFamilyName
        || contentType == KUidContactFieldSecondName
        || contentType == KUidContactFieldPhoneNumber
        || contentType == KUidContactFieldEMail
        || contentType == KUidContactFieldUrl
        || contentType == KUidContactFieldCompanyName
        || contentType == KUidContactFieldJobTitle
        || contentType == KUidContactFieldNote
        || contentType == KUidContactFieldBirthday
        || contentType == KUidContactFieldAddress
        || contentType == KUidContactFieldLocality
        || contentType == KUidContactFieldRegion
        || contentType == KUidContactFieldPostcode
        || contentType == KUidContactFieldCountry;
}

void clearManagedFieldsL(CContactItem &item)
{
    CContactItemFieldSet &fields = item.CardFields();
    // Backwards: removing shifts everything after it down.
    for (TInt i = fields.Count() - 1; i >= 0; --i) {
        if (isManagedField(fields[i].ContentType().FieldType(0)))
            fields.Remove(i);
    }
}

/// The name a contact is filed under when Google gave it no structured name: its display name
/// (which for an email-only contact is the address itself), then the organisation, then the first
/// email address.
///
/// Something has to be written. A contact with no name field at all is not one a name-sorted
/// phonebook view shows, which is why contacts holding only an email address appeared to download
/// and then not exist. The Windows Phone port has the same fallback; it was missed in this one.
QString filingNameFor(const VCardContact &c)
{
    if (!c.displayName.isEmpty()) return c.displayName;
    if (!c.organization.isEmpty()) return c.organization;
    if (!c.emails.isEmpty()) return c.emails.first().address;
    return QString();
}

/// "[GID:<uid>]" marks the contact as ours. A trailing ";n" additionally records that the name on
/// the phone was filed by this app rather than given by Google - without it, the next upload would
/// write that invented name back into the contact's N property.
QString noteWithTag(const QString &uid, const QString &note, bool synthesisedName)
{
    QString out = QLatin1String(kTagPrefix) + uid;
    if (synthesisedName) out += QLatin1String(";n");
    out += QLatin1Char(']');
    if (!note.isEmpty()) out += QLatin1Char('\n') + note;
    return out;
}

/// Splits a stored note back into the tag, the flag and the user-visible part. Tolerates the
/// earlier "[GID:<uid>]" form, which carries no flag.
void splitNote(const QString &stored, QString *uid, bool *synthesisedName, QString *note)
{
    *note = stored;
    uid->clear();
    *synthesisedName = false;

    if (!stored.startsWith(QLatin1String(kTagPrefix))) return;

    const int close = stored.indexOf(QLatin1Char(']'));
    if (close < 0) return;

    QString tag = stored.mid(5, close - 5);   // strlen("[GID:")
    if (tag.endsWith(QLatin1String(";n"))) {
        *synthesisedName = true;
        tag.chop(2);
    }
    *uid = tag;

    *note = stored.mid(close + 1);
    if (note->startsWith(QLatin1Char('\n'))) *note = note->mid(1);
}

} // anonymous namespace

// ============================================================================
PhoneContacts::PhoneContacts(const QString &dataDir)
    : m_db(0)
    , m_dataDir(dataDir)
{
    TRAPD(err, openDatabaseL());
    if (err != KErrNone) {
        m_db = 0;
        m_error = QObject::tr("The phonebook could not be opened (%1).").arg(errorText(err));
        return;
    }
    loadOwned();
}

PhoneContacts::~PhoneContacts()
{
    delete m_db;
}

void PhoneContacts::openDatabaseL()
{
    // The database normally exists; on a phone that has never had a contact it does not, and
    // creating it is what the native application would have done on first use.
    TRAPD(err, m_db = CContactDatabase::OpenL());
    if (err == KErrNotFound)
        m_db = CContactDatabase::CreateL();
    else if (err != KErrNone)
        User::Leave(err);
}

QString PhoneContacts::errorText(int symbianError)
{
    switch (symbianError) {
    case KErrNotFound:     return QLatin1String("KErrNotFound");
    case KErrPermissionDenied:
        return QLatin1String("KErrPermissionDenied - the app lacks ReadUserData/WriteUserData");
    case KErrNoMemory:     return QLatin1String("KErrNoMemory");
    case KErrInUse:        return QLatin1String("KErrInUse - another application holds the database");
    case KErrLocked:       return QLatin1String("KErrLocked");
    case KErrAccessDenied: return QLatin1String("KErrAccessDenied");
    case KErrCorrupt:      return QLatin1String("KErrCorrupt");
    case KErrDiskFull:     return QLatin1String("KErrDiskFull");
    default:               return QString::fromLatin1("error %1").arg(symbianError);
    }
}

QString PhoneContacts::diagnostics()
{
    QString text;
    text += QLatin1String("phonebook: ");
    text += isReady() ? QLatin1String("CContactDatabase open") : (QLatin1String("NOT open - ") + m_error);
    text += QLatin1Char('\n');

    if (isReady()) {
        TInt total = -1;
        TRAPD(err, total = m_db->CountL());
        text += QObject::tr("contacts in the phonebook: %1")
                    .arg(err == KErrNone ? QString::number(total) : errorText(err));
        text += QLatin1Char('\n');
    }

    text += QObject::tr("contacts owned by this app: %1").arg(m_owned.size());
    if (!m_lastWriteError.isEmpty())
        text += QLatin1Char('\n') + QObject::tr("last write error: %1").arg(m_lastWriteError);
    return text;
}

// ============================================================================
// Which contacts are ours
// ============================================================================
QString PhoneContacts::ownedPath() const
{
    QString dir = m_dataDir;
    if (!dir.endsWith(QLatin1Char('/'))) dir += QLatin1Char('/');
    return dir + QLatin1String("owned.tsv");
}

bool PhoneContacts::loadOwned()
{
    m_owned.clear();

    QFile file(ownedPath());
    if (!file.exists()) return true;
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return false;

    QTextStream in(&file);
    in.setCodec("UTF-8");
    while (!in.atEnd()) {
        const QString line = in.readLine();
        const int tab = line.indexOf(QLatin1Char('\t'));
        if (tab <= 0) continue;
        m_owned.insert(line.left(tab), line.mid(tab + 1).toUInt());
    }
    return true;
}

bool PhoneContacts::saveOwned() const
{
    QFile file(ownedPath());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) return false;

    QTextStream out(&file);
    out.setCodec("UTF-8");
    for (QMap<QString, quint32>::const_iterator it = m_owned.constBegin();
         it != m_owned.constEnd(); ++it) {
        out << it.key() << '\t' << it.value() << '\n';
    }
    return true;
}

quint32 PhoneContacts::idForUid(const QString &uid)
{
    if (uid.isEmpty() || !isReady()) return 0;

    const quint32 mapped = m_owned.value(uid, 0);
    if (mapped != 0) {
        // Confirm it is still there: the user may have deleted it in Contacts.
        TInt err = KErrNone;
        TRAP(err, {
            CContactItem *item = m_db->ReadContactLC(TContactItemId(mapped));
            CleanupStack::PopAndDestroy(item);
        });
        if (err == KErrNone) return mapped;
        m_owned.remove(uid);
    }

    // The map is gone or stale; fall back to the tag carried by the contact itself.
    quint32 found = 0;
    TRAPD(err, found = findByNoteTagL(uid));
    if (err != KErrNone) return 0;

    if (found != 0) {
        m_owned.insert(uid, found);
        saveOwned();
    }
    return found;
}

quint32 PhoneContacts::findByNoteTagL(const QString &uid)
{
    const QString needle = QLatin1String(kTagPrefix) + uid + QLatin1Char(']');

    const CContactIdArray *ids = m_db->SortedItemsL();
    if (!ids) return 0;

    for (TInt i = 0; i < ids->Count(); ++i) {
        const TContactItemId id = (*ids)[i];

        CContactItem *item = m_db->ReadContactLC(id);
        const CContactItemFieldSet &fields = item->CardFields();

        bool match = false;
        for (TInt f = 0; f < fields.Count(); ++f) {
            if (fields[f].ContentType().FieldType(0) != KUidContactFieldNote) continue;
            if (fields[f].StorageType() != KStorageTypeText) continue;
            if (fromDes(fields[f].TextStorage()->Text()).startsWith(needle)) { match = true; break; }
        }
        CleanupStack::PopAndDestroy(item);

        if (match) return id;
    }
    return 0;
}

// ============================================================================
// Google -> phone
// ============================================================================
bool PhoneContacts::write(const VCardContact &c, QString *error)
{
    if (!isReady()) { *error = m_error; return false; }
    if (c.uid.isEmpty()) {
        *error = QObject::tr("a contact with no Google UID cannot be tracked");
        return false;
    }

    TRAPD(err, writeL(c));
    if (err != KErrNone) {
        m_lastWriteError = QObject::tr("saving \"%1\" failed with %2")
                               .arg(c.displayName.isEmpty() ? c.uid : c.displayName,
                                    errorText(err));
        *error = m_lastWriteError;
        return false;
    }

    saveOwned();
    return true;
}

void PhoneContacts::writeL(const VCardContact &c)
{
    const quint32 existing = idForUid(c.uid);

    CContactItem *item = 0;
    bool isNew = false;

    if (existing != 0) {
        // Update in place, so the contact keeps its identity in the phonebook and anything
        // pointing at it (a speed dial, a call log entry) keeps working.
        item = m_db->OpenContactL(TContactItemId(existing));
        CleanupStack::PushL(item);
        clearManagedFieldsL(*item);
    } else {
        item = CContactCard::NewLC();
        isNew = true;
    }

    // When Google gave no structured name, file one from what it did give - see filingNameFor.
    // The flag in the note tag is what stops that invented name being uploaded as the real one.
    const bool haveRealName = !c.firstName.isEmpty() || !c.lastName.isEmpty();
    const QString filingName = haveRealName ? QString() : filingNameFor(c);
    const bool synthesisedName = !filingName.isEmpty();

    addTextFieldL(*item, KUidContactFieldGivenName, KUidContactFieldVCardMapUnusedN,
                  haveRealName ? c.firstName : filingName);
    addTextFieldL(*item, KUidContactFieldFamilyName, KUidContactFieldVCardMapUnusedN, c.lastName);
    addTextFieldL(*item, KUidContactFieldSecondName, KUidContactFieldVCardMapUnusedN, c.middleName);

    for (int i = 0; i < c.phones.size(); ++i) {
        const ContactPhone &p = c.phones.at(i);
        const TUid map = (p.type == QLatin1String("homefax") || p.type == QLatin1String("workfax"))
            ? KUidContactFieldVCardMapTELFAX : KUidContactFieldVCardMapTEL;
        addTextFieldL(*item, KUidContactFieldPhoneNumber, map, phoneLabelFor(p.type), p.number);
    }

    for (int i = 0; i < c.emails.size(); ++i) {
        const ContactEmail &e = c.emails.at(i);
        const TUid label = e.type == QLatin1String("work") ? KUidContactFieldVCardMapWORK
                         : e.type == QLatin1String("home") ? KUidContactFieldVCardMapHOME
                         : TUid::Uid(0);
        addTextFieldL(*item, KUidContactFieldEMail, KUidContactFieldVCardMapEMAILINTERNET,
                      label, e.address);
    }

    for (int i = 0; i < c.websites.size(); ++i)
        addTextFieldL(*item, KUidContactFieldUrl, KUidContactFieldVCardMapURL,
                      c.websites.at(i).url);

    // Symbian models one address per contact, as a set of separate fields. A second address stays
    // in the stored card and is restored on upload, so nothing is lost - it just is not on the
    // phone.
    if (!c.addresses.isEmpty()) {
        const ContactAddress &a = c.addresses.first();
        const TUid label = a.type == QLatin1String("work") ? KUidContactFieldVCardMapWORK
                         : a.type == QLatin1String("home") ? KUidContactFieldVCardMapHOME
                         : TUid::Uid(0);
        addTextFieldL(*item, KUidContactFieldAddress, KUidContactFieldVCardMapADR, label, a.street);
        addTextFieldL(*item, KUidContactFieldLocality, KUidContactFieldVCardMapADR, label, a.city);
        addTextFieldL(*item, KUidContactFieldRegion, KUidContactFieldVCardMapADR, label, a.region);
        addTextFieldL(*item, KUidContactFieldPostcode, KUidContactFieldVCardMapADR, label,
                      a.postalCode);
        addTextFieldL(*item, KUidContactFieldCountry, KUidContactFieldVCardMapADR, label, a.country);
    }

    addTextFieldL(*item, KUidContactFieldCompanyName, KUidContactFieldVCardMapORG, c.organization);
    addTextFieldL(*item, KUidContactFieldJobTitle, KUidContactFieldVCardMapTITLE, c.jobTitle);

    // The note carries the ownership tag, which is what lets a lost map be rebuilt.
    addTextFieldL(*item, KUidContactFieldNote, KUidContactFieldVCardMapNOTE,
                  noteWithTag(c.uid, c.notes, synthesisedName));

    const QDate born = QDate::fromString(c.birthday, QLatin1String("yyyy-MM-dd"));
    if (born.isValid()) {
        CContactItemField *field =
            CContactItemField::NewLC(KStorageTypeDateTime, KUidContactFieldBirthday);
        field->SetMapping(KUidContactFieldVCardMapBDAY);
        const TDateTime when(born.year(), TMonth(born.month() - 1), born.day() - 1, 0, 0, 0, 0);
        field->DateTimeStorage()->SetTime(TTime(when));
        item->AddFieldL(*field);
        CleanupStack::Pop(field);
    }

    if (isNew) {
        const TContactItemId id = m_db->AddNewContactL(*item);
        CleanupStack::PopAndDestroy(item);
        m_owned.insert(c.uid, quint32(id));
    } else {
        m_db->CommitContactL(*item);
        CleanupStack::PopAndDestroy(item);
        m_owned.insert(c.uid, existing);
    }
}

bool PhoneContacts::removeByUid(const QString &uid)
{
    if (!isReady()) return false;

    const quint32 id = idForUid(uid);
    if (id == 0) return false;

    TRAPD(err, removeL(id));
    if (err != KErrNone) return false;

    m_owned.remove(uid);
    saveOwned();
    return true;
}

void PhoneContacts::removeL(quint32 contactId)
{
    m_db->DeleteContactL(TContactItemId(contactId));
}

// ============================================================================
// Phone -> Google
// ============================================================================
bool PhoneContacts::readByUid(const QString &uid, VCardContact *out)
{
    if (!isReady()) return false;

    const quint32 id = idForUid(uid);
    if (id == 0) return false;

    TRAPD(err, readOneL(id, out));
    if (err != KErrNone) return false;

    out->uid = uid;
    return true;
}

void PhoneContacts::readOneL(quint32 contactId, VCardContact *out)
{
    CContactItem *item = m_db->ReadContactLC(TContactItemId(contactId));
    const CContactItemFieldSet &fields = item->CardFields();

    out->localId = QString::number(contactId);

    ContactAddress address;
    bool synthesisedName = false;

    for (TInt i = 0; i < fields.Count(); ++i) {
        const CContactItemField &field = fields[i];
        const TUid type = field.ContentType().FieldType(0);

        if (field.StorageType() == KStorageTypeDateTime) {
            if (type == KUidContactFieldBirthday) {
                const TTime time = field.DateTimeStorage()->Time();
                TDateTime when = time.DateTime();
                const QDate date(when.Year(), when.Month() + 1, when.Day() + 1);
                if (date.isValid()) out->birthday = date.toString(QLatin1String("yyyy-MM-dd"));
            }
            continue;
        }
        if (field.StorageType() != KStorageTypeText) continue;

        const QString value = fromDes(field.TextStorage()->Text());
        if (value.isEmpty()) continue;

        if (type == KUidContactFieldGivenName) out->firstName = value;
        else if (type == KUidContactFieldFamilyName) out->lastName = value;
        else if (type == KUidContactFieldSecondName) out->middleName = value;
        else if (type == KUidContactFieldCompanyName) out->organization = value;
        else if (type == KUidContactFieldJobTitle) out->jobTitle = value;
        else if (type == KUidContactFieldPhoneNumber)
            out->phones.append(ContactPhone(value, phoneTypeFrom(field)));
        else if (type == KUidContactFieldEMail)
            out->emails.append(ContactEmail(value, labelTypeFrom(field)));
        else if (type == KUidContactFieldUrl)
            out->websites.append(ContactWebsite(value, labelTypeFrom(field)));
        else if (type == KUidContactFieldAddress) { address.street = value; address.type = labelTypeFrom(field); }
        else if (type == KUidContactFieldLocality) address.city = value;
        else if (type == KUidContactFieldRegion) address.region = value;
        else if (type == KUidContactFieldPostcode) address.postalCode = value;
        else if (type == KUidContactFieldCountry) address.country = value;
        else if (type == KUidContactFieldNote) {
            QString tagUid, note;
            splitNote(value, &tagUid, &synthesisedName, &note);
            out->notes = note;
            if (!tagUid.isEmpty()) out->uid = tagUid;
        }
    }

    if (!address.isEmpty()) out->addresses.append(address);

    // The name in the phonebook was filed by this app, not given by Google. Reporting it as the
    // contact's name would invent one on upload, and would count as an edit - so the contact would
    // upload itself after every single sync.
    if (synthesisedName) {
        out->displayName = out->firstName;
        out->firstName.clear();
        out->lastName.clear();
    } else {
        out->displayName = (out->firstName + QLatin1Char(' ') + out->lastName).trimmed();
        if (out->displayName.isEmpty()) out->displayName = out->organization;
    }

    CleanupStack::PopAndDestroy(item);
}

bool PhoneContacts::readAll(QList<VCardContact> *out, QString *error)
{
    out->clear();
    if (!isReady()) { *error = m_error; return false; }

    TRAPD(err, readAllL(out));
    if (err != KErrNone) {
        *error = QObject::tr("the phonebook could not be read (%1)").arg(errorText(err));
        return false;
    }
    return true;
}

void PhoneContacts::readAllL(QList<VCardContact> *out)
{
    // Only our own, and the map says which those are. Every id is re-read rather than trusted,
    // because the user can delete a contact in Contacts at any time.
    const QList<QString> uids = m_owned.keys();

    for (int i = 0; i < uids.size(); ++i) {
        const QString &uid = uids.at(i);
        const quint32 id = m_owned.value(uid, 0);
        if (id == 0) continue;

        VCardContact contact;
        TRAPD(err, readOneL(id, &contact));
        if (err != KErrNone) continue;   // gone, or unreadable - the next full sync settles it

        contact.uid = uid;
        out->append(contact);
    }
}
