# GContactsSync

Sync **Google Contacts** to a **Symbian^3 / Anna / Belle** phone over **CardDAV**, using nothing but
a Google App Password. No Google Cloud project, no OAuth client, no People API.

This is the Symbian port of
[GContactsSync for Windows Phone 8.1 / Windows 10 Mobile](https://github.com/infodimus/GContactsSync-WinRT),
which is itself a port of the Windows 10 Mobile original. Same sync model, same CardDAV, same vCard
handling; everything that touches the phone is different, because Symbian's contact story is not
Windows'.

---

## What it does

* **Download (Google → phone)** — manually, and on a timer whose interval is a setting (15 minutes
  minimum). The first download fetches everything; every one after that compares stored ETags with
  the server's and touches only what changed.
* **Upload (phone → Google)** — **manual only**, from the button on the main page. Same decision the
  other ports make: a timer cannot ask the user anything, and an accidental edit should not reach
  Google unattended.
* Contacts go into the phone's own contact database, where the Contacts application, dialler and
  messaging all see them.

---

## The rule that matters

Symbian has **no per-app contact store**. Where the Windows ports get a private contact list that
the People hub merely displays, here there is one shared phonebook holding the user's real contacts.

So every contact this app creates is tagged with its Google UID (`QContactGuid`), and **the app
never reads, updates or deletes a contact that has no such tag**. A contact you typed in by hand is
invisible to every sync and cannot be overwritten or removed by one.

The cost is honest: a person who exists both in your phone and in Google stays **duplicated** until
you delete one of the two. That is the deliberate trade — the alternative, guessing which phonebook
entries "are" which Google contacts and adopting them, would let a wrong guess overwrite or delete
contacts the app never created.

---

## Getting started

### 1. A Google App Password

<https://myaccount.google.com/apppasswords> — 16 characters, and the account needs 2-Step
Verification switched on. Your normal Google password will not work; Google rejects Basic auth
with it.

### 2. Build

```
build-symbian.cmd          bumps the version, builds ARMv5 release, writes GContactsSync_<ver>.sis
build-symbian.cmd nobump   the same without bumping
build-symbian.cmd clean    removes the build output
```

Needs the **Qt 4.7.4 for Symbian Anna/Belle** kit (`SymbianSR1Qt474`) from the Nokia Qt SDK. The
version is bumped on every build on purpose: Symbian refuses to replace an installed app with an
equal or lower version.

### 3. Install

Copy `GContactsSync_<ver>.sis` to the phone and open it. It is **self-signed** in the unprotected
UID range, so it installs without Symbian Signed — the phone will warn that the supplier is
untrusted, which is expected. Allow contacts and network access when asked.

The package declares its dependencies, so a device missing Qt 4.7.4, Qt Quick Components or Qt
Mobility 1.2 says so at install time rather than failing to start. Belle has all three; Anna may
need the Smart Installer.

### 4. Sync

The main page's bottom-right button opens the menu (Settings, About, Exit). Exit closes the app
outright, rather than leaving it in the background the way the red key does - worth knowing, since
the sync timer only runs while the app is alive.

Enter the Gmail address and the App Password, tap **sync from Google**. The first sync is the long
one. Afterwards the timer takes over while the app is running, and **upload phone changes** sends
back whatever you have edited in Contacts.

#### Typing the App Password only once

**load from file** reads a two-line text file from the memory card or phone memory:

```
login: someone@gmail.com
password: abcdefghijklmnop
```

Copy it over USB as `gcontactssync.txt` — the app looks in `E:\`, `E:\Data\`, `F:\`, `F:\Data\` and
`C:\Data\`. Template: [`docs/gcontactssync.txt.sample`](docs/gcontactssync.txt.sample). It only ever
reads; delete the file afterwards, since anything on the card can read it.

---

## How it differs from the Windows ports

### No bundled OpenSSL

The Windows Phone 8.1 version ships an entire OpenSSL build inside the package, because WP8.1's TLS
stack cannot reach `www.google.com` at all. Symbian^3 / Anna / Belle negotiate modern TLS through
Qt's own stack, so there is none of that here — `QNetworkAccessManager` does the work.

The CA bundle is still shipped ([`certs/google-cacert.pem`](certs), compiled into the binary) and
added to the default `QSslConfiguration`. A 2011 device's root store predates the Google Trust
Services roots `www.google.com` chains to today, and without those roots the handshake fails on
certificate verification rather than on protocol. Adding them costs a few lines and removes the
whole question.

### Contacts have no slot limit

WP8.1's `StoredContact` is a fixed set of named slots — ten phone numbers, three email addresses,
one website — so that port carries machinery to record what would not fit and put it back on
upload. A Symbian `QContact` holds unlimited details, each with a context and a subtype, so that
machinery is simply gone here. The mapping is in
[`phonecontacts.cpp`](src/app/phonecontacts.cpp).

### The timer lives in the app

There is no equivalent of WinRT's `TimeTrigger`. The periodic download runs on a `QTimer` inside the
process: while the app is open, and while it sits in the background after the red key, which on
Symbian is where an app normally lives. With the app fully closed, nothing syncs. Fifteen minutes is
the floor — not because Symbian insists, but so all three ports behave alike.

Automatic download is **off by default** on a new install; turn it on in Settings.

**Autostart** is a setting, off by default. When switched on, the app copies a startup-list entry
into `\private\101f875a\import\` and the phone launches it at boot, so the timer runs without
anyone opening the app.

That only works on a phone whose platform security has been patched. Writing into another
application's private directory needs the `AllFiles` capability, and a self-signed package cannot
have it. `AllFiles` is deliberately **not** declared: a self-signed package that asks for it will not
install on a stock phone at all. So the app simply attempts the write, and on a stock phone the
switch refuses to stay on and says why. The switch reflects whether the entry actually exists, not
what was last chosen, so it cannot claim something the phone will not do.

Turn autostart **off before uninstalling**. The entry is created at runtime, so the installer does
not know about it and will not remove it; left behind, it points at an application that is no longer
there.

### What is the same

The CardDAV conversation, the vCard parser, the patch-on-upload design and the change fingerprint
are the same logic, ported to C++:

* an upload **patches** the card Google last sent rather than rebuilding it, so the photo,
  `CATEGORIES`, IMPP handles and every `X-` property survive a round trip
  ([`vcard.cpp`](src/core/vcard.cpp), [`syncstate.cpp`](src/core/syncstate.cpp))
* the change fingerprint is computed from the contact **read back out of the phonebook**, never
  from the card written to it ([`contacthash.cpp`](src/core/contacthash.cpp))
* `If-Match` on upload, so a contact changed on Google is reported as a conflict instead of being
  overwritten from a stale copy
* downloads batch with `addressbook-multiget` (RFC 6352 §8.7)

---

## Layout

```
GContactsSync.pro              the phone app
core.pri                       the sync core - shared, and free of UI and Qt Mobility
src/core/                      vCard, CardDAV, sync state, fingerprint, account file
src/app/                       the QtContacts bridge and the QML-facing controller
qml/                           main page, settings, about
tools/gcontactssync-cli/       desktop harness: offline checks and a real sync against Google
certs/                         the CA bundle compiled into the binary
docs/TESTING.md                what is tested where, and what needs a device
```

---

## Tests

```
build-desktop.cmd                    the offline checks
build-desktop.cmd --live <dir>       a real sync against Google, in-memory phonebook
build-desktop.cmd --live <dir> --upload
```

122 checks over the vCard parser, the patcher, the fingerprint, the account file and the sync
state — built from the **real** `src/core` sources with the Desktop Qt 4.7.4 MinGW kit, so they
cannot drift from what ships.

`--live` is the useful one: it runs the whole sync engine against your actual Google account with an
in-memory phonebook standing in for the phone, then downloads a second time and warns if that was
not a no-op — which is the failure that would otherwise rewrite the whole phonebook every fifteen
minutes on a device.

See [`docs/TESTING.md`](docs/TESTING.md) for what still needs a phone.

---

## Known limitations

* A person in both your phonebook and Google stays duplicated — see
  [the rule that matters](#the-rule-that-matters).
* Nothing syncs while the app is fully closed, unless autostart is on and the phone allows it.
* A birthday with no year (`BDAY:--0409`) does not reach the phone, as `QDate` needs one. Google
  keeps its copy, because uploads patch rather than rebuild.
* Only the organisation name reaches the phone. A department (`ORG:Acme;Sales`) is kept in the
  stored card and restored on upload as long as the company name is unchanged.
* The contact photo is downloaded but never read back from the phonebook, so replacing a photo on
  the phone does not upload it. Google's copy is preserved rather than lost.

---

## Credits

Ported from the Windows Phone 8.1 / Windows 10 Mobile version of this app, whose CardDAV model
comes from [GContactsSyncCardDAVW10M](https://github.com/infodimus/GContactsSyncCardDAVW10M) and
whose People API lineage traces to [@Computershik73](https://github.com/Computershik73).

Build arrangement and project layout follow
[Symbigram](https://github.com/infodimus/Symbigram).

This code was written with the assistance of Claude Code, Anthropic's agentic coding tool.

## Licence

GPL-3.0-or-later. See [LICENSE](LICENSE).
