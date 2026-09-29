// GContactsSync - a Google Contacts CardDAV client for Symbian^3 / Anna / Belle.
// Copyright (C) 2026 - GPL-3.0-or-later, see LICENSE.
#include "syncengine.h"

#include <QtCore/QSet>
#include <QtCore/QStringList>
#include <QtCore/QUuid>

#include "carddavclient.h"
#include "contacthash.h"
#include "contactstore.h"
#include "syncstate.h"
#include "vcard.h"

SyncEngine::SyncEngine(QObject *parent)
    : QObject(parent)
{
}

void SyncEngine::report(const QString &message)
{
    emit progress(message);
}

QString SyncEngine::contacts(int count)
{
    return count == 1 ? tr("1 contact") : tr("%1 contacts").arg(count);
}

// ============================================================================
// GOOGLE -> PHONE
// ============================================================================
SyncOutcome SyncEngine::download(CardDavClient *client, IContactStore *store, SyncState *state)
{
    SyncOutcome outcome;
    outcome.wasFullSync = !state->hasSynced();
    m_firstWriteError.clear();

    const bool ok = outcome.wasFullSync
        ? fullDownload(client, store, state, &outcome)
        : incrementalDownload(client, store, state, &outcome);

    if (!ok) {
        outcome.error = client->lastError();
        if (outcome.error.isEmpty()) outcome.error = tr("The sync could not be completed.");
        // Whatever did land is still worth keeping: the contacts are on the phone, so recording
        // their etags is what stops the next sync fetching them all over again.
        state->save();
        return outcome;
    }

    if (!state->save()) {
        outcome.error = tr("Contacts were synced, but the sync record could not be saved. "
                           "The next sync will download everything again.");
        return outcome;
    }

    outcome.success = true;
    outcome.message = describeDownload(outcome);
    if (outcome.failed > 0 && !m_firstWriteError.isEmpty()) {
        outcome.message += QLatin1Char('\n')
            + tr("%1 could not be saved. First reason: %2")
                  .arg(contacts(outcome.failed), m_firstWriteError);
    }
    return outcome;
}

bool SyncEngine::fullDownload(CardDavClient *client, IContactStore *store, SyncState *state,
                              SyncOutcome *outcome)
{
    report(tr("Asking Google for the contact list..."));

    QMap<QString, QString> listing;
    if (!client->list(&listing)) return false;

    if (listing.isEmpty()) {
        outcome->nothingToDo = true;
        return true;
    }

    report(tr("Found %1 contacts. Downloading...").arg(listing.size()));

    const QStringList hrefs = listing.keys();
    QList<FetchedCard> cards;
    if (!client->fetch(hrefs, listing, &cards)) return false;

    report(tr("Saving %1 contacts to the phone...").arg(cards.size()));

    // Every UID Google currently has, whether or not its card downloaded cleanly. Used below to
    // decide what on the phone is a stray: a contact that failed to download must not be mistaken
    // for one Google deleted. Three sources, because a card's UID need not equal the file name
    // Google chose - the cards that arrived say so themselves, a path recorded by an earlier sync
    // says so, and the file name is the last resort.
    QSet<QString> onGoogle;
    for (int i = 0; i < hrefs.size(); ++i) onGoogle.insert(SyncState::uidFromHref(hrefs.at(i)));
    for (QMap<QString, QString>::const_iterator it = state->hrefs.constBegin();
         it != state->hrefs.constEnd(); ++it) {
        if (listing.contains(it.value())) onGoogle.insert(it.key());
    }

    QSet<QString> stored;
    for (int i = 0; i < cards.size(); ++i) {
        const FetchedCard &card = cards.at(i);
        onGoogle.insert(card.contact.uid.isEmpty()
                            ? SyncState::uidFromHref(card.href) : card.contact.uid);

        if (storeCard(store, state, card)) {
            outcome->written++;
            stored.insert(card.href);
        } else {
            outcome->failed++;
        }
    }

    // Only record etags for contacts that actually reached the phone, so anything that failed is
    // picked up again by the next sync instead of being treated as current.
    state->etags.clear();
    for (QMap<QString, QString>::const_iterator it = listing.constBegin();
         it != listing.constEnd(); ++it) {
        if (stored.contains(it.key())) state->etags.insert(it.key(), it.value());
    }

    // A full download is also a reset: a contact of ours that Google no longer has must go, or one
    // deleted while this app was not syncing would live on for ever. Only ever our own contacts -
    // readAll() sees nothing the user created by hand.
    report(tr("Checking for contacts Google no longer has..."));

    QList<VCardContact> onPhone;
    QString error;
    if (store->readAll(&onPhone, &error)) {
        for (int i = 0; i < onPhone.size(); ++i) {
            const QString uid = onPhone.at(i).uid;
            if (uid.isEmpty() || onGoogle.contains(uid)) continue;

            if (store->removeByUid(uid)) outcome->removed++;
            state->deleteCard(uid);
            state->hrefs.remove(uid);
            state->hashes.remove(uid);
        }
    }
    return true;
}

bool SyncEngine::incrementalDownload(CardDavClient *client, IContactStore *store, SyncState *state,
                                     SyncOutcome *outcome)
{
    report(tr("Checking Google for changes..."));

    ServerDiff diff;
    if (!client->getChanges(state->etags, &diff)) return false;

    if (diff.changed.isEmpty() && diff.deleted.isEmpty()) {
        state->etags = diff.serverEtags;
        outcome->nothingToDo = true;
        return true;
    }

    report(tr("%1 changed, %2 removed.").arg(diff.changed.size()).arg(diff.deleted.size()));

    const QMap<QString, QString> previousEtags = state->etags;
    const QSet<QString> changedHrefs = QSet<QString>::fromList(diff.changed);
    QSet<QString> stored;

    if (!diff.changed.isEmpty()) {
        QList<FetchedCard> cards;
        if (!client->fetch(diff.changed, diff.serverEtags, &cards)) return false;

        for (int i = 0; i < cards.size(); ++i) {
            if (storeCard(store, state, cards.at(i))) {
                outcome->written++;
                stored.insert(cards.at(i).href);
            } else {
                outcome->failed++;
            }
        }
    }

    for (int i = 0; i < diff.deleted.size(); ++i) {
        const QString &href = diff.deleted.at(i);

        // The recorded map is authoritative: a UID need not match the file name Google chose.
        QString uid;
        for (QMap<QString, QString>::const_iterator it = state->hrefs.constBegin();
             it != state->hrefs.constEnd(); ++it) {
            if (it.value() == href) { uid = it.key(); break; }
        }
        if (uid.isEmpty()) uid = SyncState::uidFromHref(href);

        if (store->removeByUid(uid)) outcome->removed++;
        state->deleteCard(uid);
        state->forget(uid, href);
    }

    // Take the server's etags, but keep the previous one for any contact that did not make it onto
    // the phone - otherwise a contact that failed once would look current for ever and never be
    // retried.
    QMap<QString, QString> updated;
    for (QMap<QString, QString>::const_iterator it = diff.serverEtags.constBegin();
         it != diff.serverEtags.constEnd(); ++it) {
        const bool isChanged = changedHrefs.contains(it.key());
        if (!isChanged || stored.contains(it.key())) {
            updated.insert(it.key(), it.value());
            continue;
        }
        if (previousEtags.contains(it.key()))
            updated.insert(it.key(), previousEtags.value(it.key()));
        // A brand-new contact that failed is simply left out, so the next sync sees it as new.
    }
    state->etags = updated;
    return true;
}

bool SyncEngine::storeCard(IContactStore *store, SyncState *state, const FetchedCard &card)
{
    VCardContact contact = card.contact;

    // Google always sends a UID, but a card without one cannot be tracked at all: there would be
    // no way to match it again and every sync would create a duplicate.
    if (contact.uid.isEmpty()) contact.uid = SyncState::uidFromHref(card.href);
    if (contact.uid.isEmpty()) return false;

    QString error;
    if (!store->write(contact, &error)) {
        // Keep the first one. Without this the whole failure showed as "none could be saved to the
        // phone" with no reason at all, which is what made a phonebook problem look like a sync
        // problem for far too long.
        if (m_firstWriteError.isEmpty() && !error.isEmpty()) m_firstWriteError = error;
        return false;
    }

    // Etags are the callers' business: they alone know whether this contact's etag should now be
    // treated as current (see the comments in the two download paths).
    state->hrefs.insert(contact.uid, card.href);
    state->writeCard(contact.uid, card.card);

    VCardContact readBack;
    if (store->readByUid(contact.uid, &readBack))
        state->hashes.insert(contact.uid, ContactHash::compute(readBack));
    else
        state->hashes.remove(contact.uid);

    return true;
}

// ============================================================================
// PHONE -> GOOGLE
// ============================================================================
SyncOutcome SyncEngine::upload(CardDavClient *client, IContactStore *store, SyncState *state)
{
    SyncOutcome outcome;

    report(tr("Reading contacts from the phone..."));

    QList<VCardContact> onPhone;
    QString error;
    if (!store->readAll(&onPhone, &error)) {
        outcome.error = error.isEmpty() ? tr("The phonebook could not be read.") : error;
        return outcome;
    }

    if (onPhone.isEmpty()) {
        outcome.success = true;
        outcome.nothingToDo = true;
        outcome.message = tr("No synced contacts on the phone yet. Sync from Google first.");
        return outcome;
    }

    QList<VCardContact> changed;
    for (int i = 0; i < onPhone.size(); ++i) {
        const VCardContact &c = onPhone.at(i);

        // A contact that reads back empty is a symptom of a failed read, not an edit. Uploading it
        // would wipe the Google copy, so it is left alone.
        if (c.isEmpty()) { outcome.skipped++; continue; }

        const QString known = state->hashes.value(c.uid);
        if (!c.uid.isEmpty() && !known.isEmpty() && known == ContactHash::compute(c))
            continue;   // untouched since the last download

        changed.append(c);
    }

    if (changed.isEmpty()) {
        outcome.success = true;
        outcome.nothingToDo = true;
        outcome.message = tr("No changes on the phone to upload.");
        return outcome;
    }

    report(tr("Uploading %1 changed contact(s)...").arg(changed.size()));

    for (int i = 0; i < changed.size(); ++i) {
        if (client->wasCancelled()) {
            outcome.error = tr("Upload cancelled.");
            state->save();
            return outcome;
        }

        const VCardContact &contact = changed.at(i);

        const bool isNew = contact.uid.isEmpty();
        const QString uid = isNew
            ? QUuid::createUuid().toString().remove(QLatin1Char('{')).remove(QLatin1Char('}'))
            : contact.uid;

        const QString storedCard = state->readCard(uid);
        const QString cardText = storedCard.isEmpty()
            ? VCardWriter::buildNew(contact, uid)
            : VCardWriter::patch(storedCard, contact);

        const QString href = state->hrefs.value(uid);
        const QString knownEtag = href.isEmpty() ? QString() : state->etags.value(href);

        const UploadResult result = client->upload(uid, href, knownEtag, cardText);

        switch (result.outcome) {
        case UploadCreated:
        case UploadUpdated:
            outcome.uploaded++;
            state->hrefs.insert(uid, result.href);
            state->hashes.insert(uid, ContactHash::compute(contact));
            state->writeCard(uid, cardText);

            // The etag deliberately is NOT recorded from the PUT response. Google rewrites parts
            // of a card it accepts (REV, and its own X- properties), so letting the next download
            // see a mismatch and re-fetch is what keeps the stored card the same as the server's -
            // which is what patching depends on.
            if (!href.isEmpty()) state->etags.remove(href);

            // A contact that existed only on the phone now has a Google UID; tag it, or the next
            // upload creates a second copy.
            if (isNew) {
                VCardContact tagged = contact;
                tagged.uid = uid;
                QString ignored;
                store->write(tagged, &ignored);
            }
            break;

        case UploadConflict:
            // Left entirely alone: the stored etag still describes the card this patch was built
            // from, so the next download fetches the server's version and the user can redo the
            // edit on top of it.
            outcome.conflicts++;
            break;

        default:
            outcome.failed++;
            break;
        }

        if ((i + 1) % 5 == 0)
            report(tr("Uploaded %1 of %2...").arg(i + 1).arg(changed.size()));
    }

    if (!state->save()) {
        outcome.error = tr("Contacts were uploaded, but the sync record could not be saved.");
        return outcome;
    }

    outcome.success = true;
    outcome.message = tr("%1 of %2 changed contact(s) uploaded to Google.")
                          .arg(outcome.uploaded).arg(changed.size());
    if (outcome.conflicts > 0)
        outcome.message += QLatin1Char('\n')
            + tr("%1 changed on Google too - sync from Google, then upload again.")
                  .arg(outcome.conflicts);
    if (outcome.failed > 0)
        outcome.message += QLatin1Char('\n') + tr("%1 failed.").arg(outcome.failed);

    return outcome;
}

// ============================================================================
QString SyncEngine::describeDownload(const SyncOutcome &outcome)
{
    if (outcome.nothingToDo)
        return tr("Already up to date - nothing changed on Google.");

    if (outcome.wasFullSync)
        return tr("%1 downloaded from Google.").arg(contacts(outcome.written));

    // Google reported changes but none of them reached the phone. Saying "0 contacts updated"
    // would read as success; this did not succeed at anything.
    if (outcome.written == 0 && outcome.removed == 0)
        return tr("Google reported changes, but none could be saved to the phone. "
                  "They will be retried on the next sync.");

    QStringList parts;
    if (outcome.written > 0) parts.append(tr("%1 updated").arg(contacts(outcome.written)));
    if (outcome.removed > 0) parts.append(tr("%1 removed").arg(contacts(outcome.removed)));
    return parts.join(QLatin1String(", ")) + QLatin1Char('.');
}
