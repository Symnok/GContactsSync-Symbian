// GContactsSync - desktop harness for the sync core.
// Copyright (C) 2026 - GPL-3.0-or-later, see LICENSE.
//
// Everything in src/core is plain QtCore + QtNetwork, so all of it - the vCard parser, the
// patcher, the fingerprint, the account file, and with --live the whole sync against Google -
// can be exercised on a PC. Only the QtContacts bridge needs a phone; docs/TESTING.md lists what
// that leaves to check on the device.
#include <QtCore/QCoreApplication>
#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QMap>
#include <QtCore/QStringList>
#include <QtCore/QTextStream>

#include "carddavclient.h"
#include "contacthash.h"
#include "contactstore.h"
#include "credentialsfile.h"
#include "syncengine.h"
#include "syncstate.h"
#include "vcard.h"

static int g_failures = 0;
static QTextStream out(stdout);

static void pass(const QString &what) { out << "  ok    " << what << endl; }

static void fail(const QString &what, const QString &detail)
{
    ++g_failures;
    out << "  FAIL  " << what << ": " << detail << endl;
}

static void check(const QString &what, const QString &expected, const QString &actual)
{
    if (expected == actual) pass(what);
    else fail(what, QString::fromLatin1("expected <%1> but got <%2>").arg(expected, actual));
}

static void check(const QString &what, int expected, int actual)
{
    if (expected == actual) pass(what);
    else fail(what, QString::fromLatin1("expected %1 but got %2").arg(expected).arg(actual));
}

static void checkBool(const QString &what, bool expected, bool actual)
{
    if (expected == actual) pass(what);
    else fail(what, QString::fromLatin1("expected %1 but got %2")
                        .arg(expected ? "true" : "false").arg(actual ? "true" : "false"));
}

// ============================================================================
// A phonebook that lives in memory, so the engine can be driven without a phone
// ============================================================================
class MemoryContactStore : public IContactStore
{
public:
    QMap<QString, VCardContact> byUid;
    int writes;
    int removals;

    MemoryContactStore() : writes(0), removals(0) {}

    bool write(const VCardContact &c, QString *error)
    {
        Q_UNUSED(error)
        if (c.uid.isEmpty()) return false;

        // Stands in for what a phonebook does to a contact on the way in and out. The real
        // Symbian store drops a few things the same way - see PhoneContacts.
        VCardContact stored = c;
        stored.localId = c.uid;
        stored.etag.clear();
        stored.href.clear();
        byUid.insert(c.uid, stored);
        ++writes;
        return true;
    }

    bool removeByUid(const QString &uid)
    {
        if (!byUid.contains(uid)) return false;
        byUid.remove(uid);
        ++removals;
        return true;
    }

    bool readByUid(const QString &uid, VCardContact *result)
    {
        if (!byUid.contains(uid)) return false;
        *result = byUid.value(uid);
        return true;
    }

    bool readAll(QList<VCardContact> *result, QString *error)
    {
        Q_UNUSED(error)
        *result = byUid.values();
        return true;
    }
};

// ============================================================================
// A card shaped like the ones Google's CardDAV endpoint returns
// ============================================================================
static QString googleCard()
{
    return QLatin1String(
        "BEGIN:VCARD\r\n"
        "VERSION:3.0\r\n"
        "UID:3b1f9c2a4d5e6f70\r\n"
        "FN:Jane Q. Doe\r\n"
        "N:Doe;Jane;Quinn;Dr.;PhD\r\n"
        "NICKNAME:Janey\r\n"
        "ORG:Acme Corporation;Research\r\n"
        "TITLE:Principal Engineer\r\n"
        "item1.EMAIL;TYPE=INTERNET,HOME:jane@example.com\r\n"
        "item1.X-ABLabel:_$!<Home>!$_\r\n"
        "EMAIL;TYPE=INTERNET,WORK:jane.doe@acme.example\r\n"
        "TEL;TYPE=CELL,VOICE:+1 (800) 555-0100\r\n"
        "TEL;TYPE=HOME,VOICE:+18005550111\r\n"
        "TEL;TYPE=WORK,VOICE:+18005550122\r\n"
        "TEL;TYPE=HOME,FAX:+18005550133\r\n"
        "ADR;TYPE=HOME:;;12 Elm Street;Springfield;IL;62704;USA\r\n"
        "URL;TYPE=WORK:https://acme.example/jane\r\n"
        "URL:https://jane.example\r\n"
        "BDAY:1978-04-09\r\n"
        "NOTE:Met at the 2019 conference\\nLikes strong coffee\r\n"
        "CATEGORIES:myContacts,Colleagues\r\n"
        "X-GOOGLE-TALK:jane@talk.example\r\n"
        "PHOTO;ENCODING=b;TYPE=JPEG:/9j/4AAQSkZJRgABAQEAYABgAAD/2wBDAAgGBgcGBQgHBwcJ\r\n"
        " CQkKDAsJCQsMDQ0MDA0NDQwMDQ0NDQwNDQ0NDQ0NDQ0NDQ0NDQ0NDQ0NDQ0NDQ0NDQ0NDf/AABEI\r\n"
        " AAEAAQMBIgACEQEDEQH/xAAfAAABBQEBAQEBAQAAAAAAAAAAAQIDBAUGBwgJCgv/\r\n"
        "REV:2024-01-15T10:22:33Z\r\n"
        "END:VCARD\r\n");
}

// Unfolds a card and looks for a whole line, or counts lines containing some text. The writer
// folds at 75 octets, so a search for a value has to unfold first.
static bool hasLine(const QString &card, const QString &line)
{
    return VCardText::unfold(card).contains(line);
}

static int countStartingWith(const QString &card, const QString &prefix)
{
    const QStringList lines = VCardText::unfold(card);
    int count = 0;
    for (int i = 0; i < lines.size(); ++i)
        if (lines.at(i).startsWith(prefix)) ++count;
    return count;
}

static int countContaining(const QString &card, const QString &text)
{
    const QStringList lines = VCardText::unfold(card);
    int count = 0;
    for (int i = 0; i < lines.size(); ++i)
        if (lines.at(i).contains(text)) ++count;
    return count;
}

// ============================================================================
// Parsing
// ============================================================================
static void parsesAGoogleCard()
{
    const VCardContact c = VCardParser::parse(googleCard());

    check(QLatin1String("uid"), QLatin1String("3b1f9c2a4d5e6f70"), c.uid);
    check(QLatin1String("display name"), QLatin1String("Jane Q. Doe"), c.displayName);
    check(QLatin1String("first name"), QLatin1String("Jane"), c.firstName);
    check(QLatin1String("last name"), QLatin1String("Doe"), c.lastName);
    check(QLatin1String("middle name"), QLatin1String("Quinn"), c.middleName);
    check(QLatin1String("prefix"), QLatin1String("Dr."), c.namePrefix);
    check(QLatin1String("suffix"), QLatin1String("PhD"), c.nameSuffix);
    check(QLatin1String("nickname"), QLatin1String("Janey"), c.nickname);
    check(QLatin1String("organisation (department dropped)"),
          QLatin1String("Acme Corporation"), c.organization);
    check(QLatin1String("job title"), QLatin1String("Principal Engineer"), c.jobTitle);
    check(QLatin1String("birthday"), QLatin1String("1978-04-09"), c.birthday);
    check(QLatin1String("notes keep the newline"),
          QLatin1String("Met at the 2019 conference\nLikes strong coffee"), c.notes);

    check(QLatin1String("email count"), 2, c.emails.size());
    check(QLatin1String("grouped email survives the itemN prefix"),
          QLatin1String("jane@example.com"), c.emails.at(0).address);
    check(QLatin1String("grouped email type"), QLatin1String("home"), c.emails.at(0).type);
    check(QLatin1String("work email type"), QLatin1String("work"), c.emails.at(1).type);

    check(QLatin1String("phone count"), 4, c.phones.size());
    check(QLatin1String("mobile type"), QLatin1String("mobile"), c.phones.at(0).type);
    check(QLatin1String("home type"), QLatin1String("home"), c.phones.at(1).type);
    check(QLatin1String("work type"), QLatin1String("work"), c.phones.at(2).type);
    check(QLatin1String("home fax type"), QLatin1String("homefax"), c.phones.at(3).type);

    check(QLatin1String("address count"), 1, c.addresses.size());
    check(QLatin1String("street"), QLatin1String("12 Elm Street"), c.addresses.at(0).street);
    check(QLatin1String("city"), QLatin1String("Springfield"), c.addresses.at(0).city);
    check(QLatin1String("region"), QLatin1String("IL"), c.addresses.at(0).region);
    check(QLatin1String("postcode"), QLatin1String("62704"), c.addresses.at(0).postalCode);
    check(QLatin1String("country"), QLatin1String("USA"), c.addresses.at(0).country);
    check(QLatin1String("address type"), QLatin1String("home"), c.addresses.at(0).type);

    check(QLatin1String("website count"), 2, c.websites.size());
    check(QLatin1String("website type"), QLatin1String("work"), c.websites.at(0).type);
    check(QLatin1String("untyped website"), QLatin1String("other"), c.websites.at(1).type);

    checkBool(QLatin1String("no X-ABLabel leaked into notes"),
              true, !c.notes.contains(QLatin1String("_$!")));
}

static void unfoldsAndKeepsPhoto()
{
    const VCardContact c = VCardParser::parse(googleCard());

    checkBool(QLatin1String("photo was read"), true, !c.photoBase64.isEmpty());
    check(QLatin1String("photo type"), QLatin1String("JPEG"), c.photoType);
    checkBool(QLatin1String("photo is unfolded into one run"),
              true, !c.photoBase64.contains(QLatin1Char(' ')));
    checkBool(QLatin1String("photo starts with the JPEG marker"),
              true, c.photoBase64.startsWith(QLatin1String("/9j/4AAQSkZJRg")));
    checkBool(QLatin1String("photo carries all three folded lines"),
              true, c.photoBase64.endsWith(QLatin1String("BwgJCgv/")));
}

static void handlesVCard21StyleTypes()
{
    // Some Google accounts still hand back bare parameters rather than TYPE=.
    const QString card = QLatin1String(
        "BEGIN:VCARD\r\nVERSION:3.0\r\nUID:x\r\nFN:Old Style\r\nN:Style;Old;;;\r\n"
        "TEL;WORK;VOICE:+15550001\r\n"
        "TEL;CELL:+15550002\r\n"
        "TEL;HOME;FAX:+15550003\r\n"
        "EMAIL;INTERNET;WORK:old@example.com\r\n"
        "END:VCARD\r\n");

    const VCardContact c = VCardParser::parse(card);
    check(QLatin1String("bare WORK"), QLatin1String("work"), c.phones.at(0).type);
    check(QLatin1String("bare CELL"), QLatin1String("mobile"), c.phones.at(1).type);
    check(QLatin1String("bare HOME + FAX"), QLatin1String("homefax"), c.phones.at(2).type);
    check(QLatin1String("bare INTERNET + WORK email"), QLatin1String("work"), c.emails.at(0).type);
}

static void handlesEscaping()
{
    VCardContact c;
    c.lastName = QLatin1String("O'Brien; Jr.");
    c.firstName = QString::fromUtf8("Se\xc3\xa1n, the \\one\\");
    c.notes = QLatin1String("line one\nline two; with a comma, and a backslash \\");
    c.displayName = QString::fromUtf8("Se\xc3\xa1n O'Brien");

    ContactAddress a;
    a.street = QLatin1String("1 Semi; Colon Way");
    a.city = QLatin1String("Comma, Town");
    a.country = QLatin1String("Back\\slashia");
    c.addresses.append(a);

    const QString card = VCardWriter::buildNew(c, QLatin1String("esc-1"));
    const VCardContact back = VCardParser::parse(card);

    check(QLatin1String("semicolon in a name"), c.lastName, back.lastName);
    check(QLatin1String("comma and backslash in a name"), c.firstName, back.firstName);
    check(QLatin1String("newline and punctuation in a note"), c.notes, back.notes);
    check(QLatin1String("semicolon in a street"), a.street, back.addresses.at(0).street);
    check(QLatin1String("comma in a city"), a.city, back.addresses.at(0).city);
    check(QLatin1String("backslash in a country"), a.country, back.addresses.at(0).country);
    check(QLatin1String("the address did not split into two"), 1, back.addresses.size());
}

// ============================================================================
// Patching - the properties the phone does not own must survive
// ============================================================================
static void patchKeepsEverythingUnmanaged()
{
    const VCardContact phone = VCardParser::parse(googleCard());
    const QString patched = VCardWriter::patch(googleCard(), phone);

    checkBool(QLatin1String("UID kept"), true,
              hasLine(patched, QLatin1String("UID:3b1f9c2a4d5e6f70")));
    checkBool(QLatin1String("CATEGORIES kept"), true,
              hasLine(patched, QLatin1String("CATEGORIES:myContacts,Colleagues")));
    checkBool(QLatin1String("X- property kept"), true,
              hasLine(patched, QLatin1String("X-GOOGLE-TALK:jane@talk.example")));
    check(QLatin1String("VERSION emitted once"), 1,
          countStartingWith(patched, QLatin1String("VERSION:")));
    check(QLatin1String("REV dropped"), 0, countStartingWith(patched, QLatin1String("REV:")));

    // The photo is the whole reason patching exists: a card rebuilt from what the phonebook holds
    // would have deleted it.
    const VCardContact reparsed = VCardParser::parse(patched);
    checkBool(QLatin1String("photo survives a patch"), true, !reparsed.photoBase64.isEmpty());
    check(QLatin1String("photo is byte-identical"),
          VCardParser::parse(googleCard()).photoBase64, reparsed.photoBase64);
    check(QLatin1String("patch is a single card"), 1, VCardParser::splitCards(patched).size());
}

static void patchRewritesManagedProperties()
{
    VCardContact phone = VCardParser::parse(googleCard());
    phone.firstName = QLatin1String("Janet");
    phone.notes = QLatin1String("Now prefers tea");
    phone.phones[0].number = QLatin1String("+18005559999");

    const QString patched = VCardWriter::patch(googleCard(), phone);
    const VCardContact after = VCardParser::parse(patched);

    check(QLatin1String("edited first name reaches the card"), QLatin1String("Janet"),
          after.firstName);
    check(QLatin1String("edited note reaches the card"), QLatin1String("Now prefers tea"),
          after.notes);
    check(QLatin1String("edited number reaches the card"), QLatin1String("+18005559999"),
          after.phones.at(0).number);
    check(QLatin1String("old note is gone"), 0,
          countContaining(patched, QLatin1String("NOTE:Met at")));
    check(QLatin1String("one N line"), 1, countStartingWith(patched, QLatin1String("N:")));
    check(QLatin1String("one FN line"), 1, countStartingWith(patched, QLatin1String("FN:")));
    check(QLatin1String("phones are not duplicated"), 4, after.phones.size());
    check(QLatin1String("types are preserved across the rewrite"), QLatin1String("homefax"),
          after.phones.at(3).type);
}

static void patchDropsDeletedValues()
{
    VCardContact phone = VCardParser::parse(googleCard());
    // The user deleted the work number and the second website on the phone.
    phone.phones.removeAt(2);
    phone.websites.removeAt(1);

    const QString patched = VCardWriter::patch(googleCard(), phone);
    const VCardContact after = VCardParser::parse(patched);

    check(QLatin1String("deleted phone is gone"), 3, after.phones.size());
    check(QLatin1String("the right phone went"), 0,
          countContaining(patched, QLatin1String("+18005550122")));
    check(QLatin1String("deleted website is gone"), 1, after.websites.size());
    check(QLatin1String("the other website stayed"), QLatin1String("https://acme.example/jane"),
          after.websites.at(0).url);
}

static void patchKeepsOrgDepartment()
{
    VCardContact phone = VCardParser::parse(googleCard());

    // Unchanged company: the department Google holds has nowhere to live on the phone, so the
    // original ORG line must be left alone rather than rewritten without it.
    const QString patched = VCardWriter::patch(googleCard(), phone);
    checkBool(QLatin1String("department kept when the company is unchanged"), true,
              hasLine(patched, QLatin1String("ORG:Acme Corporation;Research")));
    check(QLatin1String("exactly one ORG line"), 1,
          countStartingWith(patched, QLatin1String("ORG:")));

    // Changed company: the user's edit wins, and the department goes with the old value.
    phone.organization = QLatin1String("Globex");
    const QString changed = VCardWriter::patch(googleCard(), phone);
    checkBool(QLatin1String("changed company is written"), true,
              hasLine(changed, QLatin1String("ORG:Globex")));
    check(QLatin1String("stale department removed"), 0,
          countContaining(changed, QLatin1String("Research")));
    check(QLatin1String("still exactly one ORG line"), 1,
          countStartingWith(changed, QLatin1String("ORG:")));
}

static void patchDropsOrphanedLabels()
{
    const VCardContact phone = VCardParser::parse(googleCard());
    const QString patched = VCardWriter::patch(googleCard(), phone);

    // item1.EMAIL is re-emitted as a plain EMAIL, so the label that grouped with it has nothing
    // left to label. Leaving it behind gives Google a dangling label.
    check(QLatin1String("orphaned X-ABLabel removed"), 0,
          countContaining(patched, QLatin1String("X-ABLabel")));
    check(QLatin1String("its email is still there"), 1,
          countContaining(patched, QLatin1String("jane@example.com")));
}

static void roundTripsTwiceUnchanged()
{
    // Patching a card that was itself the result of a patch must not drift: that happens every
    // time the user edits the same contact twice.
    const VCardContact phone = VCardParser::parse(googleCard());
    const QString first = VCardWriter::patch(googleCard(), phone);
    const QString second = VCardWriter::patch(first, VCardParser::parse(first));

    check(QLatin1String("a second patch changes nothing"), first, second);
}

static void buildNewIsSelfContained()
{
    VCardContact c;
    c.firstName = QLatin1String("Ada");
    c.lastName = QLatin1String("Lovelace");
    c.displayName = QLatin1String("Ada Lovelace");
    c.organization = QLatin1String("Analytical Engines");
    c.birthday = QLatin1String("18151210");
    c.phones.append(ContactPhone(QLatin1String("+442079460000"), QLatin1String("work")));
    c.emails.append(ContactEmail(QLatin1String("ada@example.org"), QLatin1String("home")));

    const QString card = VCardWriter::buildNew(c, QLatin1String("new-uid-1"));

    checkBool(QLatin1String("begins"), true, card.startsWith(QLatin1String("BEGIN:VCARD\r\n")));
    checkBool(QLatin1String("ends"), true, card.trimmed().endsWith(QLatin1String("END:VCARD")));
    checkBool(QLatin1String("has the uid"), true, hasLine(card, QLatin1String("UID:new-uid-1")));
    checkBool(QLatin1String("version 3.0"), true, hasLine(card, QLatin1String("VERSION:3.0")));

    const VCardContact back = VCardParser::parse(card);
    check(QLatin1String("name round-trips"), QLatin1String("Ada"), back.firstName);
    check(QLatin1String("phone round-trips"), QLatin1String("+442079460000"),
          back.phones.at(0).number);
    check(QLatin1String("phone type round-trips"), QLatin1String("work"), back.phones.at(0).type);
    check(QLatin1String("email type round-trips"), QLatin1String("home"), back.emails.at(0).type);
    check(QLatin1String("compact birthday is normalised"), QLatin1String("1815-12-10"),
          back.birthday);
}

static void birthdayForms()
{
    VCardContact c;
    c.firstName = QLatin1String("B");
    c.lastName = QLatin1String("Day");

    c.birthday = QLatin1String("--0409");
    checkBool(QLatin1String("year-less birthday passes through"), true,
              hasLine(VCardWriter::buildNew(c, QLatin1String("b1")),
                      QLatin1String("BDAY:--0409")));

    c.birthday = QLatin1String("19780409");
    checkBool(QLatin1String("compact birthday is normalised"), true,
              hasLine(VCardWriter::buildNew(c, QLatin1String("b2")),
                      QLatin1String("BDAY:1978-04-09")));

    c.birthday = QLatin1String("nonsense");
    check(QLatin1String("unusable birthday is dropped"), 0,
          countStartingWith(VCardWriter::buildNew(c, QLatin1String("b3")),
                            QLatin1String("BDAY")));
}

// ============================================================================
// The fingerprint
// ============================================================================
static void hashIsStableAndSensitive()
{
    const VCardContact a = VCardParser::parse(googleCard());
    VCardContact b = VCardParser::parse(googleCard());
    check(QLatin1String("same input, same fingerprint"),
          ContactHash::compute(a), ContactHash::compute(b));

    // Reformatting a number is not an edit.
    b.phones[0].number = QLatin1String("+18005550100");
    check(QLatin1String("phone formatting is ignored"),
          ContactHash::compute(a), ContactHash::compute(b));

    // Case and padding in a name are not edits either.
    b = VCardParser::parse(googleCard());
    b.firstName = QLatin1String("  JANE  ");
    check(QLatin1String("name case and padding are ignored"),
          ContactHash::compute(a), ContactHash::compute(b));

    b = VCardParser::parse(googleCard());
    b.phones[0].number = QLatin1String("+18005559999");
    checkBool(QLatin1String("a changed number shows"), true,
              ContactHash::compute(a) != ContactHash::compute(b));

    b = VCardParser::parse(googleCard());
    b.notes = QLatin1String("different");
    checkBool(QLatin1String("a changed note shows"), true,
              ContactHash::compute(a) != ContactHash::compute(b));

    b = VCardParser::parse(googleCard());
    b.phones[0].type = QLatin1String("work");
    checkBool(QLatin1String("a changed label shows"), true,
              ContactHash::compute(a) != ContactHash::compute(b));

    b = VCardParser::parse(googleCard());
    b.phones.removeAt(1);
    checkBool(QLatin1String("a removed number shows"), true,
              ContactHash::compute(a) != ContactHash::compute(b));
}

// ============================================================================
// The account file
// ============================================================================
static void credentialsFileIsForgiving()
{
    // The format as documented, spacing included.
    CredentialsFileResult r = CredentialsFile::parse(QLatin1String(
        "login:someone@gmail.com\r\npassword: abcdefghijklmnop\r\n"));
    check(QLatin1String("login read"), QLatin1String("someone@gmail.com"), r.email);
    check(QLatin1String("password read"), QLatin1String("abcdefghijklmnop"), r.password);
    checkBool(QLatin1String("complete"), true, r.isComplete());
    checkBool(QLatin1String("no complaint"), true, r.problem.isEmpty());

    // LF only, blank lines, comments, odd spacing, mixed-case keys, '=' as the separator.
    r = CredentialsFile::parse(QLatin1String(
        "# my test account\n\n  LOGIN  :  someone@gmail.com  \n"
        "; and the app password\nPassword=abcdefghijklmnop\n\n"));
    check(QLatin1String("survives comments and spacing"), QLatin1String("someone@gmail.com"),
          r.email);
    check(QLatin1String("accepts = as the separator"), QLatin1String("abcdefghijklmnop"),
          r.password);

    // Google shows an App Password as four groups of four, and copying it that way is the obvious
    // thing to do.
    r = CredentialsFile::parse(QLatin1String(
        "login: someone@gmail.com\npassword: abcd efgh ijkl mnop\n"));
    check(QLatin1String("spaces are stripped from the password"),
          QLatin1String("abcdefghijklmnop"), r.password);
    checkBool(QLatin1String("and it counts as 16"), true, r.problem.isEmpty());

    r = CredentialsFile::parse(QLatin1String(
        "email: first.last+tag@gmail.com\npass: abcdefghijklmnop\n"));
    check(QLatin1String("aliases work"), QLatin1String("first.last+tag@gmail.com"), r.email);

    // And the ways it can be wrong, each of which should say so rather than fail at Google.
    check(QLatin1String("empty file is reported"), QLatin1String("the file is empty"),
          CredentialsFile::parse(QString()).problem);
    check(QLatin1String("nonsense is reported"),
          QLatin1String("no \"login:\" or \"password:\" line found"),
          CredentialsFile::parse(QLatin1String("this is not a config file\n")).problem);
    check(QLatin1String("missing password is reported"),
          QLatin1String("no \"password:\" line found"),
          CredentialsFile::parse(QLatin1String("login: someone@gmail.com\n")).problem);
    check(QLatin1String("missing login is reported"),
          QLatin1String("no \"login:\" line found"),
          CredentialsFile::parse(QLatin1String("password: abcdefghijklmnop\n")).problem);
    check(QLatin1String("a login that is not an address is reported"),
          QLatin1String("the login line does not look like an email address"),
          CredentialsFile::parse(QLatin1String(
              "login: someone\npassword: abcdefghijklmnop\n")).problem);
    // The mistake this is really here to catch: the account password, not an App Password.
    check(QLatin1String("a password of the wrong length is reported"),
          QLatin1String("the password is 7 characters; a Google App Password is 16"),
          CredentialsFile::parse(QLatin1String(
              "login: someone@gmail.com\npassword: hunter2\n")).problem);
}

// ============================================================================
// Sync state
// ============================================================================
static void syncStatePersists()
{
    QString dir = QDir::temp().absoluteFilePath(QLatin1String("gcs-selftest"));
    // Start from a clean slate without needing Qt 5's removeRecursively().
    {
        QDir d(dir + QLatin1String("/cards"));
        const QStringList files = d.entryList(QDir::Files);
        for (int i = 0; i < files.size(); ++i) d.remove(files.at(i));
    }
    QFile::remove(dir + QLatin1String("/etags.tsv"));
    QFile::remove(dir + QLatin1String("/hrefs.tsv"));
    QFile::remove(dir + QLatin1String("/hashes.tsv"));

    {
        SyncState state(dir);
        checkBool(QLatin1String("a fresh state has never synced"), false, state.hasSynced());

        state.etags.insert(QLatin1String("/carddav/a.vcf"), QLatin1String("\"etag-a\""));
        state.hrefs.insert(QLatin1String("uid-a"), QLatin1String("/carddav/a.vcf"));
        state.hashes.insert(QLatin1String("uid-a"), QLatin1String("deadbeefdeadbeef"));
        state.writeCard(QLatin1String("uid-a"), googleCard());
        checkBool(QLatin1String("saved"), true, state.save());
    }
    {
        SyncState state(dir);
        checkBool(QLatin1String("loaded"), true, state.load());
        checkBool(QLatin1String("now it has synced"), true, state.hasSynced());
        check(QLatin1String("etag survives a reload"), QLatin1String("\"etag-a\""),
              state.etags.value(QLatin1String("/carddav/a.vcf")));
        check(QLatin1String("href survives a reload"), QLatin1String("/carddav/a.vcf"),
              state.hrefs.value(QLatin1String("uid-a")));
        check(QLatin1String("fingerprint survives a reload"), QLatin1String("deadbeefdeadbeef"),
              state.hashes.value(QLatin1String("uid-a")));
        check(QLatin1String("the cached card survives a reload"), googleCard(),
              state.readCard(QLatin1String("uid-a")));

        state.forget(QLatin1String("uid-a"), QLatin1String("/carddav/a.vcf"));
        checkBool(QLatin1String("forget drops the etag"), false,
                  state.etags.contains(QLatin1String("/carddav/a.vcf")));
        checkBool(QLatin1String("forget drops the href"), false,
                  state.hrefs.contains(QLatin1String("uid-a")));
    }

    check(QLatin1String("a UID is recovered from a path"), QLatin1String("3b1f9c2a"),
          SyncState::uidFromHref(
              QLatin1String("/carddav/v1/principals/me/lists/default/3b1f9c2a.vcf")));
    check(QLatin1String("and percent-decoded"), QLatin1String("a b"),
          SyncState::uidFromHref(QLatin1String("/carddav/a%20b.vcf")));
}

// ============================================================================
// Unlabelled phone numbers
// ============================================================================
// A number Google holds with no label, or labelled Other, is not a mobile. Filing it as one put
// "Mobile" against numbers on the phone that were nothing of the kind.
static void unlabelledNumbersAreNotMobiles()
{
    const QString card = QLatin1String(
        "BEGIN:VCARD\r\nVERSION:3.0\r\nUID:u1\r\nFN:No Labels\r\nN:Labels;No;;;\r\n"
        "TEL:+15550000\r\n"                    // no TYPE at all
        "TEL;TYPE=OTHER:+15550001\r\n"         // explicitly Other
        "TEL;TYPE=VOICE:+15550002\r\n"         // a medium, not a label
        "TEL;TYPE=PREF:+15550003\r\n"          // a preference, not a label
        "TEL;TYPE=CELL,VOICE:+15550004\r\n"    // a real mobile, for contrast
        "END:VCARD\r\n");

    const VCardContact c = VCardParser::parse(card);
    check(QLatin1String("five numbers parsed"), 5, c.phones.size());
    check(QLatin1String("no TYPE is not a mobile"), QLatin1String("other"), c.phones.at(0).type);
    check(QLatin1String("TYPE=OTHER is other"), QLatin1String("other"), c.phones.at(1).type);
    check(QLatin1String("TYPE=VOICE is other"), QLatin1String("other"), c.phones.at(2).type);
    check(QLatin1String("TYPE=PREF is other"), QLatin1String("other"), c.phones.at(3).type);
    check(QLatin1String("a real CELL is still a mobile"),
          QLatin1String("mobile"), c.phones.at(4).type);

    // On the way back an unlabelled number keeps no TYPE, rather than being given one it never
    // had. Uploading it must not invent "Mobile" on Google either.
    const QString written = VCardWriter::patch(card, c);
    checkBool(QLatin1String("an unlabelled number goes back unlabelled"), true,
              hasLine(written, QLatin1String("TEL:+15550000")));
    check(QLatin1String("and no CELL is invented anywhere"), 1,
          countContaining(written, QLatin1String("CELL")));
    check(QLatin1String("the real mobile keeps its label"), 1,
          countContaining(written, QLatin1String("TEL;TYPE=CELL,VOICE:+15550004")));

    // And the round trip settles: parsing what was written gives the same types back.
    const VCardContact again = VCardParser::parse(written);
    check(QLatin1String("types survive a round trip"), 5, again.phones.size());
    for (int i = 0; i < c.phones.size(); ++i)
        check(QString::fromLatin1("number %1 keeps its type").arg(i),
              c.phones.at(i).type, again.phones.at(i).type);
    check(QLatin1String("and so does the fingerprint"),
          ContactHash::compute(c), ContactHash::compute(again));
}

// ============================================================================
// Request URLs
// ============================================================================
// The bug this exists to prevent: every URL CardDavClient builds is already percent-encoded, and
// QUrl(QString) treats its argument as UNencoded - so it escaped the '%' of an existing escape and
// asked Google for "someone%2540gmail.com". Google has no principal by that name, so it answered
// 404 and the app reported the address book as missing. On the phone that looked like a wrong
// address; it was a mangled URL.
static void requestUrlsAreNotDoubleEncoded()
{
    CardDavClient client(QLatin1String("someone@gmail.com"), QLatin1String("abcdefghijklmnop"));

    const QString collection = client.collectionUrl();
    checkBool(QLatin1String("the address is percent-encoded in the collection path"),
              true, collection.contains(QLatin1String("someone%40gmail.com")));

    const QByteArray onTheWire = CardDavClient::toRequestUrl(collection).toEncoded();
    checkBool(QLatin1String("and reaches the wire encoded exactly once"),
              true, onTheWire.contains("someone%40gmail.com"));
    checkBool(QLatin1String("never double-encoded"),
              false, onTheWire.contains("%2540"));

    // The same rule protects the hrefs Google hands back, which arrive already encoded.
    const QByteArray href = CardDavClient::toRequestUrl(
        QLatin1String("https://www.google.com/carddav/v1/a%20b/c%40d.vcf")).toEncoded();
    checkBool(QLatin1String("an encoded space in an href survives"), true, href.contains("a%20b"));
    checkBool(QLatin1String("an encoded at-sign in an href survives"), true, href.contains("c%40d"));
    checkBool(QLatin1String("and neither is escaped again"), false, href.contains("%25"));

    // A UID with characters that must be escaped still produces one level of escaping.
    const QString newHref = client.hrefForNewUid(QLatin1String("uid with space"));
    checkBool(QLatin1String("a new contact's path is encoded"),
              true, newHref.contains(QLatin1String("uid%20with%20space")));
    checkBool(QLatin1String("and not double-encoded on the wire"), false,
              CardDavClient::toRequestUrl(QLatin1String("https://www.google.com") + newHref)
                  .toEncoded().contains("%2520"));
}

// ============================================================================
// Live sync
// ============================================================================
static int liveSync(const QString &dataDir, bool doUpload)
{
    const CredentialsFileResult account = CredentialsFile::load();
    if (!account.isComplete()) {
        out << "no usable account file found." << endl;
        out << "looked in:" << endl;
        const QStringList paths = CredentialsFile::searchPaths();
        for (int i = 0; i < paths.size(); ++i) out << "  " << paths.at(i) << endl;
        if (!account.problem.isEmpty()) out << "problem: " << account.problem << endl;
        return 2;
    }
    out << "account: " << account.email << "  (from " << account.source << ")" << endl;

    if (!CardDavClient::sslAvailable()) {
        out << "this Qt build has no SSL support - cannot reach Google." << endl;
        return 2;
    }

    QDir().mkpath(dataDir);

    CardDavClient client(account.email, account.password);
    SyncEngine engine;
    MemoryContactStore store;
    SyncState state(dataDir);
    state.load();

    QObject::connect(&engine, SIGNAL(progress(QString)), &client, SIGNAL(progress(QString)));

    out << (state.hasSynced() ? "incremental download..." : "first (full) download...") << endl;
    SyncOutcome down = engine.download(&client, &store, &state);
    out << (down.success ? "OK:    " : "ERROR: ") << (down.success ? down.message : down.error)
        << endl;
    out << "       written=" << down.written << " removed=" << down.removed
        << " failed=" << down.failed << " contacts in memory=" << store.byUid.size() << endl;
    if (!down.success) return 1;

    // What actually arrived. A sync that files every name but no numbers shows up here as
    // "with phone=0" rather than as an empty phonebook entry on the device.
    {
        int withPhone = 0, withEmail = 0, withAddress = 0, withOrg = 0, withNote = 0, cards = 0;
        for (QMap<QString, VCardContact>::const_iterator it = store.byUid.constBegin();
             it != store.byUid.constEnd(); ++it) {
            if (!it.value().phones.isEmpty()) ++withPhone;
            if (!it.value().emails.isEmpty()) ++withEmail;
            if (!it.value().addresses.isEmpty()) ++withAddress;
            if (!it.value().organization.isEmpty()) ++withOrg;
            if (!it.value().notes.isEmpty()) ++withNote;
            if (!state.readCard(it.key()).isEmpty()) ++cards;
        }
        out << "       with phone=" << withPhone << " email=" << withEmail
            << " address=" << withAddress << " org=" << withOrg << " note=" << withNote
            << " stored cards=" << cards << endl;
        if (!store.byUid.isEmpty()) {
            const QString first = store.byUid.keys().first();
            out << "--- stored card of " << store.byUid.value(first).displayName << " ---" << endl
                << state.readCard(first) << endl << "---" << endl;
        }
    }

    // A second download must find nothing to do. If it does find work, the etag bookkeeping or the
    // fingerprint is wrong, and on a real phone that would mean re-writing the whole phonebook
    // every fifteen minutes.
    out << "second download (should be a no-op)..." << endl;
    const int writesBefore = store.writes;
    SyncOutcome again = engine.download(&client, &store, &state);
    out << (again.success ? "OK:    " : "ERROR: ") << (again.success ? again.message : again.error)
        << endl;
    if (again.success && store.writes != writesBefore)
        out << "WARNING: the second download rewrote " << (store.writes - writesBefore)
            << " contact(s) - incremental sync is not settling." << endl;

    if (!doUpload) return down.success && again.success ? 0 : 1;

    if (store.byUid.isEmpty()) {
        out << "nothing downloaded, so nothing to upload." << endl;
        return 0;
    }

    // Touch the note of one contact and upload it. The note is chosen because it is the least
    // disruptive field to change on a real account.
    const QString uid = store.byUid.keys().first();
    VCardContact edited = store.byUid.value(uid);
    const QString marker = QLatin1String("GContactsSync harness ")
                         + QString::number(QDateTime::currentDateTime().toTime_t());
    edited.notes = marker;
    store.byUid.insert(uid, edited);
    out << "edited the note of " << edited.displayName << " and uploading..." << endl;

    SyncOutcome up = engine.upload(&client, &store, &state);
    out << (up.success ? "OK:    " : "ERROR: ") << (up.success ? up.message : up.error) << endl;
    out << "       uploaded=" << up.uploaded << " conflicts=" << up.conflicts
        << " failed=" << up.failed << endl;

    // And prove the patch preserved what the phone cannot hold.
    const QString stored = state.readCard(uid);
    out << "       stored card still has PHOTO: "
        << (stored.contains(QLatin1String("PHOTO")) ? "yes" : "no/none") << endl;

    return up.success ? 0 : 1;
}

// ============================================================================
int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    const QStringList args = app.arguments();

    // --echo <origin>: run one PROPFIND against a local server instead of Google, so the exact
    // request this client emits can be read off the wire. No account is involved.
    if (args.contains(QLatin1String("--echo"))) {
        const int at = args.indexOf(QLatin1String("--echo"));
        const QString origin = at + 1 < args.size() ? args.at(at + 1)
                                                    : QLatin1String("http://127.0.0.1:8088");
        CardDavClient client(QLatin1String("someone@gmail.com"),
                             QLatin1String("abcdefghijklmnop"), origin);
        QMap<QString, QString> etags;
        const bool ok = client.list(&etags);
        out << (ok ? "list() returned true" : "list() returned false: " + client.lastError())
            << endl;
        return 0;
    }

    // --parse <file>: run real vCards (e.g. saved with curl) through the parser and print what
    // the phone would be given. Needs no network and no TLS.
    if (args.contains(QLatin1String("--parse"))) {
        const int at = args.indexOf(QLatin1String("--parse"));
        QFile file(at + 1 < args.size() ? args.at(at + 1) : QString());
        if (!file.open(QIODevice::ReadOnly)) { out << "cannot open file" << endl; return 2; }
        const QStringList cards = VCardParser::splitCards(QString::fromUtf8(file.readAll()));
        for (int i = 0; i < cards.size(); ++i) {
            const VCardContact c = VCardParser::parse(cards.at(i));
            out << c.displayName << " | first=" << c.firstName << " last=" << c.lastName
                << " phones=" << c.phones.size() << " emails=" << c.emails.size()
                << " addresses=" << c.addresses.size() << endl;
            for (int p = 0; p < c.phones.size(); ++p)
                out << "    tel type=" << c.phones.at(p).type << endl;
        }
        return 0;
    }

    if (args.contains(QLatin1String("--live"))) {
        const int at = args.indexOf(QLatin1String("--live"));
        const QString dir = at + 1 < args.size() && !args.at(at + 1).startsWith(QLatin1String("--"))
            ? args.at(at + 1)
            : QDir::temp().absoluteFilePath(QLatin1String("gcs-live"));
        return liveSync(dir, args.contains(QLatin1String("--upload")));
    }

    parsesAGoogleCard();
    unfoldsAndKeepsPhoto();
    handlesVCard21StyleTypes();
    handlesEscaping();
    patchKeepsEverythingUnmanaged();
    patchRewritesManagedProperties();
    patchDropsDeletedValues();
    patchKeepsOrgDepartment();
    patchDropsOrphanedLabels();
    roundTripsTwiceUnchanged();
    buildNewIsSelfContained();
    birthdayForms();
    hashIsStableAndSensitive();
    credentialsFileIsForgiving();
    requestUrlsAreNotDoubleEncoded();
    unlabelledNumbersAreNotMobiles();
    syncStatePersists();

    out << endl << (g_failures == 0 ? "ALL TESTS PASSED"
                                    : QString::fromLatin1("%1 CHECK(S) FAILED").arg(g_failures))
        << endl;
    return g_failures == 0 ? 0 : 1;
}
