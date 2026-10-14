// GContactsSync - a Google Contacts CardDAV client for Symbian^3 / Anna / Belle.
// Copyright (C) 2026 - GPL-3.0-or-later, see LICENSE.
//
// See phonecontacts.h for why this is native Symbian C++ and not Qt Mobility.
#include "phonecontacts.h"

#include <QtCore/QDate>
#include <QtCore/QDateTime>
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

/// The vCard parameters that make up a field's label. Anything else in a content type (VOICE,
/// PREF...) describes the medium, not the label, and is left to whatever the template says.
enum LabelBit { LabelHome = 1, LabelWork = 2, LabelCell = 4, LabelFax = 8, LabelPager = 16,
                LabelVideo = 32, LabelCar = 64 };

int labelsOf(const CContentType &type)
{
    int bits = 0;
    if (type.ContainsFieldType(KUidContactFieldVCardMapHOME)) bits |= LabelHome;
    if (type.ContainsFieldType(KUidContactFieldVCardMapWORK)) bits |= LabelWork;
    if (type.ContainsFieldType(KUidContactFieldVCardMapCELL)) bits |= LabelCell;
    if (type.ContainsFieldType(KUidContactFieldVCardMapFAX)) bits |= LabelFax;
    if (type.ContainsFieldType(KUidContactFieldVCardMapPAGER)) bits |= LabelPager;
    if (type.ContainsFieldType(KUidContactFieldVCardMapVIDEO)) bits |= LabelVideo;
    if (type.ContainsFieldType(KUidContactFieldVCardMapCAR)) bits |= LabelCar;
    return bits;
}

/// The system template's field for this content type, mapping and label, or 0.
const CContactItemField *templateFieldFor(const CContactItem *tmpl, TStorageType storage,
                                          TUid contentType, TUid mapping, int labels)
{
    if (!tmpl) return 0;
    const CContactItemFieldSet &fields = tmpl->CardFields();
    for (TInt i = 0; i < fields.Count(); ++i) {
        const CContactItemField &f = fields[i];
        const CContentType &type = f.ContentType();
        if (f.StorageType() != storage || type.FieldTypeCount() == 0) continue;
        if (type.FieldType(0) != contentType || type.Mapping() != mapping) continue;
        if (labelsOf(type) != labels) continue;
        return &f;
    }
    return 0;
}

/// A new, empty field the Contacts application will recognise.
///
/// Contacts shows a field properly only when it matches one of its own field types, and building
/// fields by hand got that wrong in ways a desktop test cannot see (a field dump from a phone did):
/// numbers without VOICE, fax as a TELFAX mapping instead of the Fax content type, city / region /
/// postcode / country all mapped as ADR, middle name stored as Nickname. So the field is copied
/// from the system template - the definition Contacts itself uses - and built by hand, following
/// the template's conventions, only when the template has no such field.
CContactItemField *newFieldLC(const CContactItem *tmpl, TStorageType storage, TUid contentType,
                              TUid mapping, int labels)
{
    const CContactItemField *model = templateFieldFor(tmpl, storage, contentType, mapping, labels);
    if (model) {
        CContactItemField *field = CContactItemField::NewLC(*model);
        field->SetTemplateField(EFalse);
        return field;
    }

    CContactItemField *field = CContactItemField::NewLC(storage, contentType);
    field->SetMapping(mapping);
    if (contentType == KUidContactFieldPhoneNumber
        && !(labels & (LabelCell | LabelPager | LabelVideo)))
        field->AddFieldTypeL(KUidContactFieldVCardMapVOICE);
    if (labels & LabelCell) field->AddFieldTypeL(KUidContactFieldVCardMapCELL);
    if (labels & LabelPager) field->AddFieldTypeL(KUidContactFieldVCardMapPAGER);
    if (labels & LabelFax) field->AddFieldTypeL(KUidContactFieldVCardMapFAX);
    if (labels & LabelVideo) field->AddFieldTypeL(KUidContactFieldVCardMapVIDEO);
    if (labels & LabelCar) field->AddFieldTypeL(KUidContactFieldVCardMapCAR);
    if (labels & LabelHome) field->AddFieldTypeL(KUidContactFieldVCardMapHOME);
    if (labels & LabelWork) field->AddFieldTypeL(KUidContactFieldVCardMapWORK);
    return field;
}

/// Adds one text field. Empty values add nothing.
void addTextFieldL(CContactItem &item, const CContactItem *tmpl, TUid contentType, TUid mapping,
                   int labels, const QString &value)
{
    if (value.isEmpty()) return;

    CContactItemField *field = newFieldLC(tmpl, KStorageTypeText, contentType, mapping, labels);
    field->TextStorage()->SetTextL(toPtr(value));

    // AddFieldL takes ownership, so the field is popped rather than destroyed.
    item.AddFieldL(*field);
    CleanupStack::Pop(field);
}

/// The label of a home / work / anything-else value.
int homeWorkLabel(const QString &type)
{
    if (type == QLatin1String("home")) return LabelHome;
    if (type == QLatin1String("work")) return LabelWork;
    return 0;
}

/// A phone number's content type, from our normalised type. Faxes are a content type of their
/// own on Symbian, not a kind of phone number.
TUid phoneContentTypeFor(const QString &type)
{
    return type == QLatin1String("homefax") || type == QLatin1String("workfax")
        ? KUidContactFieldFax : KUidContactFieldPhoneNumber;
}

int phoneLabelsFor(const QString &type)
{
    if (type == QLatin1String("mobile")) return LabelCell;
    if (type == QLatin1String("home")) return LabelHome;
    if (type == QLatin1String("work")) return LabelWork;
    if (type == QLatin1String("pager")) return LabelPager;
    if (type == QLatin1String("homefax")) return LabelFax | LabelHome;
    if (type == QLatin1String("workfax")) return LabelFax | LabelWork;
    return 0;
}

/// The inverse. It must be the exact inverse of the writer, or every download would look like
/// an edit and the next upload would push the whole address book back to Google.
QString phoneTypeFrom(const CContactItemField &field)
{
    const int labels = labelsOf(field.ContentType());
    const bool fax = (labels & LabelFax)
                  || field.ContentType().FieldType(0) == KUidContactFieldFax;

    if (fax) return (labels & LabelWork) ? QLatin1String("workfax") : QLatin1String("homefax");
    if (labels & LabelPager) return QLatin1String("pager");
    if (labels & LabelCell) return QLatin1String("mobile");
    if (labels & LabelHome) return QLatin1String("home");
    if (labels & LabelWork) return QLatin1String("work");
    return QLatin1String("other");
}

QString labelTypeFrom(const CContactItemField &field)
{
    const int labels = labelsOf(field.ContentType());
    if (labels & LabelHome) return QLatin1String("home");
    if (labels & LabelWork) return QLatin1String("work");
    return QLatin1String("other");
}

/// Every content type this app writes. A rewrite clears exactly these and nothing else: any other
/// field on the contact belongs to the user or to another application. SecondName is here only
/// because builds up to 1.0.13 wrote the middle name into it; on these phones it is Nickname.
bool isManagedField(TUid contentType)
{
    return contentType == KUidContactFieldGivenName
        || contentType == KUidContactFieldFamilyName
        || contentType == KUidContactFieldAdditionalName
        || contentType == KUidContactFieldSecondName
        || contentType == KUidContactFieldPhoneNumber
        || contentType == KUidContactFieldFax
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
        if (fields[i].ContentType().FieldTypeCount() == 0) continue;
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
    , m_template(0)
    , m_tagIndexBuiltMs(-1)
{
    TRAPD(err, openDatabaseL());
    if (err != KErrNone) {
        m_db = 0;
        m_error = QObject::tr("The phonebook could not be opened (%1).").arg(errorText(err));
        return;
    }

    // Without the template every field is built by hand instead (see newFieldLC), which is a
    // fallback, not a failure.
    TRAP(err, m_template = m_db->ReadContactL(m_db->TemplateId()));
    if (err != KErrNone) m_template = 0;

    loadOwned();
}

PhoneContacts::~PhoneContacts()
{
    delete m_template;
    delete m_db;
}

// ============================================================================
// Field layout version
// ============================================================================
// "2" = fields copied from the system template (1.0.14). No file, with contacts already owned,
// means they were written by an earlier build and need rewriting.
QString PhoneContacts::layoutPath() const
{
    QString dir = m_dataDir;
    if (!dir.endsWith(QLatin1Char('/'))) dir += QLatin1Char('/');
    return dir + QLatin1String("layout.txt");
}

bool PhoneContacts::layoutIsCurrent() const
{
    if (m_owned.isEmpty()) return true;
    QFile file(layoutPath());
    if (!file.open(QIODevice::ReadOnly)) return false;
    return file.readAll().trimmed() == "2";
}

void PhoneContacts::markLayoutCurrent()
{
    QFile file(layoutPath());
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) file.write("2\n");
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
// Field dump
// ============================================================================
// Contacts were seen to arrive with their names but nothing else showing. Whether the other fields
// are missing from the database or present but not recognised by the Contacts application can only
// be told on the phone, so this writes out how each field is actually stored: content type,
// mapping, label, flags. Values are given as lengths only - it reads hand-made contacts too.
namespace {

QString uidHex(TUid uid)
{
    return QLatin1String("0x") + QString::number(quint32(uid.iUid), 16);
}

void dumpItemL(CContactDatabase &db, TContactItemId id, const QString &title, QString *out)
{
    CContactItem *item = db.ReadContactLC(id);
    *out += QString::fromLatin1("=== %1  id=%2 type=%3 templateRef=%4 fields=%5\n")
                .arg(title).arg(id).arg(uidHex(item->Type()))
                .arg(item->TemplateRefId()).arg(item->CardFields().Count());

    const CContactItemFieldSet &fields = item->CardFields();
    for (TInt i = 0; i < fields.Count(); ++i) {
        const CContactItemField &f = fields[i];
        const CContentType &ct = f.ContentType();

        QStringList types;
        for (TInt t = 0; t < ct.FieldTypeCount(); ++t) types.append(uidHex(ct.FieldType(t)));

        QString value;
        if (f.StorageType() == KStorageTypeText)
            value = QString::fromLatin1("text len=%1").arg(f.TextStorage()->Text().Length());
        else if (f.StorageType() == KStorageTypeDateTime)
            value = QLatin1String("datetime");
        else
            value = QString::fromLatin1("storage=%1").arg(f.StorageType());

        *out += QString::fromLatin1("  [%1] types=%2 map=%3 label=\"%4\" id=%5 "
                                    "hidden=%6 disabled=%7 useradded=%8 %9\n")
                    .arg(i).arg(types.join(QLatin1String(","))).arg(uidHex(ct.Mapping()))
                    .arg(fromDes(f.Label())).arg(f.Id())
                    .arg(f.IsHidden() ? 1 : 0).arg(f.IsDisabled() ? 1 : 0)
                    .arg(f.UserAddedField() ? 1 : 0).arg(value);
    }
    CleanupStack::PopAndDestroy(item);
}

} // anonymous namespace

QString PhoneContacts::dumpFields(const QString &path)
{
    if (!isReady()) return m_error;

    QString text = diagnostics() + QLatin1Char('\n');

    // The system template first: it is how the Contacts application itself describes each field.
    TRAPD(err, dumpItemL(*m_db, m_db->TemplateId(), QLatin1String("system template"), &text));
    if (err != KErrNone) text += QString::fromLatin1("template: %1\n").arg(errorText(err));

    // Three of ours.
    int shown = 0;
    for (QMap<QString, quint32>::const_iterator it = m_owned.constBegin();
         it != m_owned.constEnd() && shown < 3; ++it, ++shown) {
        TRAP(err, dumpItemL(*m_db, TContactItemId(it.value()), QLatin1String("synced"), &text));
        if (err != KErrNone) text += QString::fromLatin1("synced %1: %2\n").arg(it.value())
                                         .arg(errorText(err));
    }

    // And up to three made by hand, for comparison: those are known to display properly.
    QList<quint32> ours = m_owned.values();
    shown = 0;
    TRAP(err, {
        const CContactIdArray *ids = m_db->SortedItemsL();
        for (TInt i = 0; ids && i < ids->Count() && shown < 3; ++i) {
            if (ours.contains(quint32((*ids)[i]))) continue;
            dumpItemL(*m_db, (*ids)[i], QLatin1String("not synced"), &text);
            ++shown;
        }
    });
    if (err != KErrNone) text += QString::fromLatin1("hand-made: %1\n").arg(errorText(err));

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return QString();
    file.write(text.toUtf8());
    return path;
}

// ============================================================================
// Phonebook repair
// ============================================================================
namespace {

/// The ids of every card template except the system one.
QList<TContactItemId> strayTemplateIdsL(CContactDatabase &db)
{
    QList<TContactItemId> out;
    CContactIdArray *ids = db.GetCardTemplateIdListL();
    if (!ids) return out;
    CleanupStack::PushL(ids);
    for (TInt i = 0; i < ids->Count(); ++i) {
        if ((*ids)[i] != db.TemplateId()) out.append((*ids)[i]);
    }
    CleanupStack::PopAndDestroy(ids);
    return out;
}

/// A card template's name: its label field, else whatever text its name fields hold.
QString templateNameL(CContactDatabase &db, TContactItemId id)
{
    QString name;
    CContactItem *item = db.ReadContactLC(id);
    const CContactItemFieldSet &fields = item->CardFields();
    for (TInt i = 0; i < fields.Count() && name.isEmpty(); ++i) {
        if (fields[i].StorageType() != KStorageTypeText) continue;
        name = fromDes(fields[i].TextStorage()->Text()).trimmed();
    }
    CleanupStack::PopAndDestroy(item);
    return name;
}

} // anonymous namespace

QString PhoneContacts::strayTemplates()
{
    if (!isReady()) return m_error;

    QString text;
    TRAPD(err, {
        const QList<TContactItemId> ids = strayTemplateIdsL(*m_db);
        for (int i = 0; i < ids.size(); ++i) {
            const QString name = templateNameL(*m_db, ids.at(i));
            text += QString::fromLatin1("%1: %2\n").arg(ids.at(i))
                        .arg(name.isEmpty() ? QObject::tr("(no name)") : name);
        }
    });
    if (err != KErrNone) return QObject::tr("could not be listed (%1)").arg(errorText(err));
    return text.isEmpty() ? QObject::tr("none") : text.trimmed();
}

QString PhoneContacts::repairPhonebook()
{
    if (!isReady()) return m_error;

    QStringList done;

    if (m_db->IsDamaged()) {
        TRAPD(err, m_db->RecoverL());
        done.append(err == KErrNone ? QObject::tr("the database reported damage and was recovered")
                                    : QObject::tr("the database reported damage; recovery failed "
                                                  "(%1)").arg(errorText(err)));
    }

    QList<TContactItemId> ids;
    TRAPD(err, ids = strayTemplateIdsL(*m_db));
    if (err != KErrNone)
        done.append(QObject::tr("card templates could not be listed (%1)").arg(errorText(err)));

    for (int i = 0; i < ids.size(); ++i) {
        TRAP(err, m_db->DeleteContactL(ids.at(i)));
        done.append(err == KErrNone
            ? QObject::tr("card template %1 deleted").arg(ids.at(i))
            : QObject::tr("card template %1 could not be deleted (%2)")
                  .arg(ids.at(i)).arg(errorText(err)));
    }
    if (ids.isEmpty() && err == KErrNone) done.append(QObject::tr("no stray card templates"));

    TRAP(err, m_db->CompactL());
    done.append(err == KErrNone ? QObject::tr("database compacted")
                                : QObject::tr("compacting failed (%1)").arg(errorText(err)));

    m_tagIndexBuiltMs = -1;
    return done.join(QLatin1String("\n"));
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
    // One pass over the phonebook serves every lookup for a minute. Scanning per contact made a
    // first sync of a few hundred contacts read the phonebook a few hundred times over.
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (m_tagIndexBuiltMs < 0 || now - m_tagIndexBuiltMs > 60000) {
        buildTagIndexL();
        m_tagIndexBuiltMs = now;
    }
    return m_tagIndex.value(uid, 0);
}

void PhoneContacts::buildTagIndexL()
{
    m_tagIndex.clear();

    const CContactIdArray *ids = m_db->SortedItemsL();
    if (!ids) return;

    for (TInt i = 0; i < ids->Count(); ++i) {
        const TContactItemId id = (*ids)[i];

        CContactItem *item = m_db->ReadContactLC(id);
        const CContactItemFieldSet &fields = item->CardFields();

        for (TInt f = 0; f < fields.Count(); ++f) {
            if (fields[f].ContentType().FieldTypeCount() == 0) continue;
            if (fields[f].ContentType().FieldType(0) != KUidContactFieldNote) continue;
            if (fields[f].StorageType() != KStorageTypeText) continue;

            QString tagUid, note;
            bool synthesised = false;
            splitNote(fromDes(fields[f].TextStorage()->Text()), &tagUid, &synthesised, &note);
            if (!tagUid.isEmpty()) m_tagIndex.insert(tagUid, quint32(id));
            break;
        }
        CleanupStack::PopAndDestroy(item);
    }
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

    // Linked to the system template, as every contact the Contacts application creates is. Cards
    // written by builds up to 1.0.13 had no template at all; an update fixes that too.
    if (m_template) item->SetTemplateRefId(m_db->TemplateId());

    // When Google gave no structured name, file one from what it did give - see filingNameFor.
    // The flag in the note tag is what stops that invented name being uploaded as the real one.
    const bool haveRealName = !c.firstName.isEmpty() || !c.lastName.isEmpty();
    const QString filingName = haveRealName ? QString() : filingNameFor(c);
    const bool synthesisedName = !filingName.isEmpty();

    const CContactItem *tmpl = m_template;
    const TUid nameMap = KUidContactFieldVCardMapUnusedN;

    addTextFieldL(*item, tmpl, KUidContactFieldGivenName, nameMap, 0,
                  haveRealName ? c.firstName : filingName);
    addTextFieldL(*item, tmpl, KUidContactFieldFamilyName, nameMap, 0, c.lastName);
    addTextFieldL(*item, tmpl, KUidContactFieldAdditionalName, nameMap, 0, c.middleName);

    for (int i = 0; i < c.phones.size(); ++i) {
        const ContactPhone &p = c.phones.at(i);
        addTextFieldL(*item, tmpl, phoneContentTypeFor(p.type), KUidContactFieldVCardMapTEL,
                      phoneLabelsFor(p.type), p.number);
    }

    for (int i = 0; i < c.emails.size(); ++i) {
        const ContactEmail &e = c.emails.at(i);
        addTextFieldL(*item, tmpl, KUidContactFieldEMail, KUidContactFieldVCardMapEMAILINTERNET,
                      homeWorkLabel(e.type), e.address);
    }

    for (int i = 0; i < c.websites.size(); ++i) {
        const ContactWebsite &w = c.websites.at(i);
        addTextFieldL(*item, tmpl, KUidContactFieldUrl, KUidContactFieldVCardMapURL,
                      homeWorkLabel(w.type), w.url);
    }

    // Symbian models one address per contact, as a set of separate fields, each with a mapping of
    // its own. A second address stays in the stored card and is restored on upload, so nothing is
    // lost - it just is not on the phone.
    if (!c.addresses.isEmpty()) {
        const ContactAddress &a = c.addresses.first();
        const int label = homeWorkLabel(a.type);
        addTextFieldL(*item, tmpl, KUidContactFieldAddress, KUidContactFieldVCardMapADR, label,
                      a.street);
        addTextFieldL(*item, tmpl, KUidContactFieldLocality, KUidContactFieldVCardMapLOCALITY,
                      label, a.city);
        addTextFieldL(*item, tmpl, KUidContactFieldRegion, KUidContactFieldVCardMapREGION,
                      label, a.region);
        addTextFieldL(*item, tmpl, KUidContactFieldPostcode, KUidContactFieldVCardMapPOSTCODE,
                      label, a.postalCode);
        addTextFieldL(*item, tmpl, KUidContactFieldCountry, KUidContactFieldVCardMapCOUNTRY,
                      label, a.country);
    }

    addTextFieldL(*item, tmpl, KUidContactFieldCompanyName, KUidContactFieldVCardMapORG, 0,
                  c.organization);
    addTextFieldL(*item, tmpl, KUidContactFieldJobTitle, KUidContactFieldVCardMapTITLE, 0,
                  c.jobTitle);

    // The note carries the ownership tag, which is what lets a lost map be rebuilt.
    addTextFieldL(*item, tmpl, KUidContactFieldNote, KUidContactFieldVCardMapNOTE, 0,
                  noteWithTag(c.uid, c.notes, synthesisedName));

    const QDate born = QDate::fromString(c.birthday, QLatin1String("yyyy-MM-dd"));
    if (born.isValid()) {
        CContactItemField *field = newFieldLC(tmpl, KStorageTypeDateTime, KUidContactFieldBirthday,
                                              KUidContactFieldVCardMapBDAY, 0);
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
        if (field.ContentType().FieldTypeCount() == 0) continue;
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
        else if (type == KUidContactFieldAdditionalName) out->middleName = value;
        else if (type == KUidContactFieldCompanyName) out->organization = value;
        else if (type == KUidContactFieldJobTitle) out->jobTitle = value;
        else if (type == KUidContactFieldPhoneNumber || type == KUidContactFieldFax)
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
