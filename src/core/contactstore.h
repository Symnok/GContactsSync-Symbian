// GContactsSync - a Google Contacts CardDAV client for Symbian^3 / Anna / Belle.
// Copyright (C) 2026 - GPL-3.0-or-later, see LICENSE.
//
// What the sync engine needs from a phonebook, and nothing more.
//
// An interface rather than a direct call into QtContacts for two reasons. It keeps the engine
// free of Qt Mobility, so the desktop harness can drive the whole of it against an in-memory
// store - which is the only way to test sync behaviour without a phone. And it states the one
// rule the Symbian implementation must honour: Symbian has no per-app contact store, only the
// single shared phonebook, so every method here is defined to see ONLY contacts this app created
// and tagged with a Google UID. Anything the user added by hand is invisible to it and can never
// be updated or deleted by a sync.
#ifndef CONTACTSTORE_H
#define CONTACTSTORE_H

#include <QtCore/QList>
#include <QtCore/QString>

#include "vcard.h"

class IContactStore
{
public:
    virtual ~IContactStore() {}

    /// Creates or updates the phone contact for a downloaded card, matched on its Google UID.
    /// Returns false and sets *error when that one contact could not be written.
    virtual bool write(const VCardContact &c, QString *error) = 0;

    /// Removes the contact tagged with this UID. False when the phonebook had none.
    virtual bool removeByUid(const QString &uid) = 0;

    /// Reads back the contact tagged with this UID. False when there is none.
    ///
    /// Used immediately after write(), because the fingerprint that decides "the user edited
    /// this" has to be taken from what the phonebook actually kept, not from what it was handed.
    virtual bool readByUid(const QString &uid, VCardContact *out) = 0;

    /// Every contact this app owns - that is, every contact carrying a Google UID tag.
    virtual bool readAll(QList<VCardContact> *out, QString *error) = 0;
};

#endif // CONTACTSTORE_H
