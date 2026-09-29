# Testing

## What runs on the desktop

```
build-desktop.cmd                            the offline checks
build-desktop.cmd --live <dir>               a real sync against Google
build-desktop.cmd --live <dir> --upload      as above, then edit one contact and upload it
```

Built from the **real** `src/core` sources with the Desktop Qt 4.7.4 MinGW kit — nothing in
`tools/gcontactssync-cli` is a copy, so the checks cannot drift from what ships. That is possible
because `core.pri` is plain QtCore + QtNetwork: the phonebook is reached through `IContactStore`, and
the harness supplies an in-memory one.

### Offline: 122 checks, no account needed

| Area | What is checked |
|---|---|
| `VCardParser` | a realistic Google card: `itemN.` groups, `X-ABLabel`, folded base64 photos, vCard 2.1 bare type parameters (`TEL;WORK;VOICE`), `ORG` with a department, escaping |
| `VCardText` | line unfolding, escape/unescape round trips, structured values split on unescaped semicolons only |
| `VCardWriter::patch` | photo / `CATEGORIES` / `X-` properties survive; managed properties rewritten once, not duplicated; deleted values go; orphaned `X-ABLabel` lines go with their value; `ORG` departments; patching a patch changes nothing |
| `VCardWriter::buildNew` | a self-contained card for a phone-only contact; birthday forms |
| `ContactHash` | the same card gives the same fingerprint; phone-number formatting and name case are not edits; a changed number, note, label or count is |
| `CredentialsFile` | `:` or `=`, any spacing and key case, comments, CRLF or LF, spaces stripped from the App Password, and a useful complaint for each way it can be wrong |
| `SyncState` | the maps and the cached card survive a reload; the card is **byte-identical** (it is stored as raw bytes, not through a text stream — line-ending translation would corrupt the CRLF framing that patching depends on); UIDs recovered from a path |

### Live: the whole engine, against your real account

`--live` runs `SyncEngine` end to end with an in-memory phonebook. It is the closest thing to a
device test that does not need a device, and it checks the one thing unit tests cannot:

> After a successful download it downloads **again** and warns if that was not a no-op.

That second pass is the important one. If the ETag bookkeeping or the fingerprint is wrong, the
second download rewrites contacts — and on a phone that means rewriting the whole phonebook every
fifteen minutes, which is the worst failure this app can have.

`--upload` then edits one contact's note and uploads it, exercising the patch path against the real
server, and reports whether the stored card still carries its `PHOTO` afterwards.

Both write only to the directory you name, plus — with `--upload` — one note field on one real
Google contact. Use a test account if that matters.

## What needs a device

Only the QtContacts bridge and the UI. In this order:

### 1. It installs and starts

The package is self-signed in the unprotected UID range, so the phone warns about an untrusted
supplier; that is expected. Grant contacts and network access when asked.

A failure to start is almost always a missing dependency — Qt 4.7.4, Qt Quick Components, or Qt
Mobility 1.2. The `.pkg` declares all three, so the installer should say which.

### 2. TLS reaches Google

Tap **sync from Google** with a deliberately wrong password. The right failure is Google's 401 with
the App Password explanation. The wrong ones:

| Message | Cause |
|---|---|
| `Could not reach Google: ...` mentioning SSL or certificate | the trust bundle did not load, or the device's TLS cannot negotiate with Google. This is the assumption the port is built on, so it is the first thing to check. |
| `This Qt build has no TLS support` | `QSslSocket::supportsSsl()` is false — the device's Qt has no SSL backend, and nothing here can work |
| `the request timed out` | no data connection, or the access point prompt was dismissed |

### 3. The contacts rule

**The most important check in this document.** Before the first sync, note a contact you created by
hand on the phone. After a full sync, and again after a second one:

* it is still there, unmodified
* it was not deleted by the "contacts Google no longer has" pass
* the Google contacts have arrived alongside it

If a hand-made contact is ever touched, the `QContactGuid` tagging is not doing its job and the sync
should not be used until it is.

### 4. Incremental download settles

Sync, then sync again with nothing changed on either side. The second must report *"Already up to
date"*. If it reports contacts updated, the fingerprint taken from the phonebook read-back differs
from the one taken at write time — the same failure `--live` watches for, but the phonebook's own
normalisation is what would cause it here, and only a device can show that.

Then change one contact on Google and sync: exactly that one should be reported.

Delete one on Google and sync: it should disappear from the phone.

### 5. Upload

Edit a synced contact in the phone's Contacts application, then **upload phone changes**:

* only that contact is uploaded
* uploading again immediately reports no changes
* on Google the edit is there **and** the photo, categories and anything else the phone cannot hold
  are still there

For the conflict path: edit the same contact on both sides without syncing in between, then upload.
It should be reported as changed on Google, not overwritten.

### 6. The timer

Settings → automatic download on, 15 minutes. Leave the app open, or press the red key to send it to
the background, and come back. The main page's status line shows the last download time.

With the app **closed** nothing syncs, by design.

### 7. A large phonebook

Several hundred contacts is where this port's differences bite: `readAll()` loads every contact to
find the tagged ones, and the multiget response for twenty cards with photos is held in memory at
once. Watch for the sync completing at all, and for how long the first download takes.
