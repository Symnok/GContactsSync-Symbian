// GContactsSync - a Google Contacts CardDAV client for Symbian^3 / Anna / Belle.
// Copyright (C) 2026 - GPL-3.0-or-later, see LICENSE.
//
// A fingerprint of a contact as the phonebook holds it, which is how "the user edited this one"
// is detected for the phone-to-Google direction: after a download the fingerprint of every
// contact is recorded, and an upload only considers contacts whose fingerprint has moved.
//
// The one rule that matters: both sides of the comparison must be computed from a contact *read
// back out of the phonebook*, never from the card that was written to it. Field ordering and the
// phonebook's own normalisation mean the two differ, and hashing the written form would make
// every contact look edited on the very next upload. SyncEngine therefore re-reads what it has
// just written.
#ifndef CONTACTHASH_H
#define CONTACTHASH_H

#include <QtCore/QString>

class VCardContact;

namespace ContactHash
{
/// A stable fingerprint of the fields the phonebook can hold.
QString compute(const VCardContact &c);
}

#endif // CONTACTHASH_H
