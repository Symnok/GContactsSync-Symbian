// GContactsSync - a Google Contacts CardDAV client for Symbian^3 / Anna / Belle.
// Copyright (C) 2026 - GPL-3.0-or-later, see LICENSE.
#include "appcontroller.h"

#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QSettings>
#include <QtCore/QTimer>
#include <QtGui/QDesktopServices>

#include "autostart.h"
#include "carddavclient.h"
#include "credentialsfile.h"
#include "phonecontacts.h"
#include "syncengine.h"
#include "syncstate.h"

// Stringified by the compiler from the .pro file's VERSION.
#define STRINGIFY_(x) #x
#define STRINGIFY(x) STRINGIFY_(x)

namespace {
const char * const kOrganisation = "GContactsSync";
const char * const kApplication = "GContactsSync";
}

AppController::AppController(QObject *parent)
    : QObject(parent)
    , m_contacts(0)
    , m_timer(new QTimer(this))
    , m_autoSyncEnabled(false)
    , m_intervalMinutes(MinimumIntervalMinutes)
    , m_busy(false)
    , m_cancelRequested(false)
    , m_resultIsError(false)
    , m_syncedCount(0)
    , m_client(0)
{
    installTrustBundle();

    loadSettings();

    // Opening the phonebook is what asks Symbian for contact access, so it happens once here
    // rather than on every sync.
    m_contacts = new PhoneContacts(dataDir());
    if (!m_contacts->isReady())
        setResult(m_contacts->error(), true);

    {
        SyncState state(dataDir());
        state.load();
        m_syncedCount = state.hrefs.size();
    }

    m_timer->setSingleShot(false);
    connect(m_timer, SIGNAL(timeout()), this, SLOT(onTimer()));
    applyTimer();
}

AppController::~AppController()
{
    delete m_contacts;
}

QString AppController::version() const
{
    return QLatin1String(STRINGIFY(APP_VERSION));
}

QString AppController::dataDir() const
{
    // The app's own private directory. On Symbian that is under C:\Data\... for this app only, so
    // the account file and the cached cards are not world-readable.
    const QString dir = QDesktopServices::storageLocation(QDesktopServices::DataLocation);
    QDir().mkpath(dir);
    return dir;
}

void AppController::installTrustBundle()
{
    // Symbian^3 / Anna / Belle negotiate modern TLS through Qt's own stack, so there is no bundled
    // OpenSSL here as there is in the Windows Phone version. The CA bundle is still shipped and
    // added to the default configuration, because a 2011 device's root store predates the Google
    // Trust Services roots that www.google.com chains to today - without it the handshake fails on
    // certificate verification, not on protocol.
    QFile bundle(QLatin1String(":/certs/google-cacert.pem"));
    if (!bundle.open(QIODevice::ReadOnly)) return;
    CardDavClient::installTrustBundle(bundle.readAll());
}

// ============================================================================
// Settings
// ============================================================================
// Not "QSettings settings(QLatin1String(a), QLatin1String(b))": with both arguments written as
// function-style casts that parses as a function declaration, and every use of it then fails on a
// non-class type. The named locals are what disambiguate it.
void AppController::loadSettings()
{
    const QString organisation = QLatin1String(kOrganisation);
    const QString application = QLatin1String(kApplication);
    QSettings settings(organisation, application);
    m_email = settings.value(QLatin1String("email")).toString();
    m_password = settings.value(QLatin1String("password")).toString();
    // Off until asked for: a fresh install should not start reaching the network on a timer
    // before anyone has said it may.
    m_autoSyncEnabled = settings.value(QLatin1String("autoSync"), false).toBool();
    m_intervalMinutes = settings.value(QLatin1String("intervalMinutes"),
                                       MinimumIntervalMinutes).toInt();
    if (m_intervalMinutes < MinimumIntervalMinutes) m_intervalMinutes = MinimumIntervalMinutes;
    m_lastDownload = settings.value(QLatin1String("lastDownload")).toString();
    m_lastUpload = settings.value(QLatin1String("lastUpload")).toString();
}

void AppController::saveSettings() const
{
    const QString organisation = QLatin1String(kOrganisation);
    const QString application = QLatin1String(kApplication);
    QSettings settings(organisation, application);
    settings.setValue(QLatin1String("email"), m_email);
    settings.setValue(QLatin1String("password"), m_password);
    settings.setValue(QLatin1String("autoSync"), m_autoSyncEnabled);
    settings.setValue(QLatin1String("intervalMinutes"), m_intervalMinutes);
    settings.setValue(QLatin1String("lastDownload"), m_lastDownload);
    settings.setValue(QLatin1String("lastUpload"), m_lastUpload);
    settings.sync();
}

// Both setters ignore an empty value, and that is a guard rather than a nicety: a QML field being
// torn down reports itself as empty, which is how exiting the app once wiped the saved account.
// Clearing the account deliberately goes through forgetAccount().
void AppController::setEmail(const QString &value)
{
    const QString cleaned = value.trimmed();
    if (cleaned.isEmpty() || m_email == cleaned) return;

    m_email = cleaned;
    saveSettings();
    emit accountChanged();
}

void AppController::setPassword(const QString &value)
{
    // Google shows an App Password as four groups of four; it is not what Google accepts.
    const QString cleaned = QString(value).remove(QLatin1Char(' ')).trimmed();
    if (cleaned.isEmpty() || m_password == cleaned) return;

    m_password = cleaned;
    saveSettings();
    emit accountChanged();
}

void AppController::setAutoSyncEnabled(bool value)
{
    if (m_autoSyncEnabled == value) return;
    m_autoSyncEnabled = value;
    saveSettings();
    applyTimer();
    emit settingsChanged();
}

void AppController::setIntervalMinutes(int value)
{
    const int clamped = value < MinimumIntervalMinutes ? MinimumIntervalMinutes : value;
    if (m_intervalMinutes == clamped) return;
    m_intervalMinutes = clamped;
    saveSettings();
    applyTimer();
    emit settingsChanged();
}

void AppController::applyTimer()
{
    m_timer->stop();
    if (!m_autoSyncEnabled) return;

    // Only once a sync has actually worked: the first one is the long download, it is the one that
    // fails on a bad App Password, and the user should be watching when it happens.
    if (m_lastDownload.isEmpty()) return;

    m_timer->start(m_intervalMinutes * 60 * 1000);
}

// ============================================================================
// State shown in the UI
// ============================================================================
void AppController::setBusy(bool value)
{
    if (m_busy == value) return;
    m_busy = value;
    emit busyChanged();
}

void AppController::setStatus(const QString &value)
{
    m_status = value;
    emit statusChanged();
}

void AppController::setResult(const QString &value, bool isError)
{
    m_result = value;
    m_resultIsError = isError;
    emit resultChanged();
}

bool AppController::autostartEnabled() const
{
    return Autostart::isEnabled();
}

void AppController::setAutostartEnabled(bool value)
{
    if (value == Autostart::isEnabled()) return;

    QString error;
    const bool ok = value ? Autostart::enable(&error) : Autostart::disable(&error);

    if (ok) {
        setResult(value
            ? tr("GContactsSync will start when the phone is switched on.")
            : tr("GContactsSync will no longer start with the phone."), false);
    } else {
        setResult(error, true);
    }

    // Emitted either way: on failure this is what snaps the switch back to what is actually true.
    emit settingsChanged();
}

QString AppController::intervalText() const
{
    if (m_intervalMinutes < 60)
        return tr("%1 minutes").arg(m_intervalMinutes);
    if (m_intervalMinutes == 60)
        return tr("1 hour");
    if (m_intervalMinutes % 60 == 0)
        return tr("%1 hours").arg(m_intervalMinutes / 60);

    // Not reachable from the picker, which offers whole hours only, but a value left by an older
    // build or edited by hand should still read sensibly.
    return tr("%1 h %2 min").arg(m_intervalMinutes / 60).arg(m_intervalMinutes % 60);
}

QString AppController::syncedSummary() const
{
    if (m_syncedCount == 0) return tr("nothing synced yet");
    return m_syncedCount == 1 ? tr("1 contact synced from Google")
                              : tr("%1 contacts synced from Google").arg(m_syncedCount);
}

QString AppController::aboutText() const
{
    QString text = tr("GContactsSync %1").arg(version()) + QLatin1Char('\n');
    text += tr("Google Contacts over CardDAV, with a Google App Password. No Google Cloud project "
               "and no OAuth client needed.") + QLatin1Char('\n');
    text += tr("Contacts this app created carry their Google UID, and it never touches a contact "
               "that does not - anything you added by hand is left alone.");
    if (m_contacts) {
        text += QLatin1Char('\n');
        text += m_contacts->diagnostics();
    }
    if (!CardDavClient::sslAvailable()) {
        text += QLatin1Char('\n');
        text += tr("WARNING: this Qt build reports no TLS support, so Google cannot be reached.");
    }
    return text;
}

void AppController::dumpContactFields()
{
    if (!m_contacts) return;

    const char * const paths[] = { "E:/gcontactssync-dump.txt", "F:/gcontactssync-dump.txt",
                                   "C:/Data/gcontactssync-dump.txt" };
    for (unsigned i = 0; i < sizeof paths / sizeof paths[0]; ++i) {
        const QString written = m_contacts->dumpFields(QLatin1String(paths[i]));
        if (!written.isEmpty()) {
            setResult(tr("Contact fields written to %1").arg(QDir::toNativeSeparators(written)),
                      false);
            return;
        }
    }
    setResult(tr("The field dump could not be written."), true);
}

QString AppController::strayTemplatesText()
{
    return m_contacts ? m_contacts->strayTemplates() : tr("The phonebook is not available.");
}

void AppController::repairPhonebook()
{
    if (!m_contacts || m_busy) return;
    setResult(m_contacts->repairPhonebook()
                  + QLatin1Char('\n') + tr("Restart the phone before opening Contacts."),
              false);
}

void AppController::onProgress(const QString &message)
{
    setStatus(message);
}

void AppController::onTimer()
{
    if (m_busy) return;   // the next tick will do
    runSync(Download, true);
}

void AppController::cancel()
{
    m_cancelRequested = true;
    if (m_client) m_client->cancel();
    setStatus(tr("Stopping..."));
}

// ============================================================================
// Sync
// ============================================================================
bool AppController::haveUsableAccount(QString *why) const
{
    if (m_email.isEmpty() || !m_email.contains(QLatin1Char('@'))) {
        *why = tr("Enter the Gmail address of the account to sync.");
        return false;
    }
    if (m_password.size() < 16) {
        *why = tr("Enter the 16-character Google App Password for this account - not the "
                  "account's own password.");
        return false;
    }
    return true;
}

void AppController::runSync(Direction direction, bool fromTimer)
{
    // One at a time. The HTTP waits run a nested event loop, so without this a second tap during a
    // sync would start a second one on top of the first, both writing the same state files.
    if (m_busy) {
        if (!fromTimer) setResult(tr("A sync is already running."), true);
        return;
    }

    QString why;
    if (!haveUsableAccount(&why)) {
        if (!fromTimer) setResult(why, true);
        return;
    }
    if (!m_contacts || !m_contacts->isReady()) {
        if (!fromTimer)
            setResult(m_contacts ? m_contacts->error() : tr("The phonebook is not available."), true);
        return;
    }
    if (!CardDavClient::sslAvailable()) {
        if (!fromTimer)
            setResult(tr("This Qt build has no TLS support, so Google cannot be reached."), true);
        return;
    }

    setBusy(true);
    m_cancelRequested = false;
    setResult(QString(), false);
    setStatus(fromTimer ? tr("Automatic sync...") : tr("Starting..."));

    CardDavClient client(m_email, m_password);
    m_client = &client;

    SyncEngine engine;
    connect(&client, SIGNAL(progress(QString)), this, SLOT(onProgress(QString)));
    connect(&engine, SIGNAL(progress(QString)), this, SLOT(onProgress(QString)));

    SyncState state(dataDir());
    state.load();

    // Contacts written by 1.0.13 and earlier have hand-built fields the Contacts application
    // mostly ignores. Forgetting the etags makes this a full download, which rewrites each of them
    // in place in the current layout. The forgetting is saved and the layout marked current at
    // once: a contact the rewrite has not reached yet has no etag, so if the sync is cut short the
    // next one carries on from there rather than starting the whole rewrite again.
    if (direction == Download && !m_contacts->layoutIsCurrent()) {
        state.etags.clear();
        state.save();
        m_contacts->markLayoutCurrent();
        setStatus(tr("Updating the contacts already on the phone..."));
    }

    const SyncOutcome outcome = direction == Download
        ? engine.download(&client, m_contacts, &state)
        : engine.upload(&client, m_contacts, &state);

    m_client = 0;
    m_syncedCount = state.hrefs.size();

    const QString when = QDateTime::currentDateTime().toString(QLatin1String("d MMM yyyy hh:mm"));
    if (outcome.success) {
        if (direction == Download) {
            m_lastDownload = when;
            if (outcome.failed == 0) m_contacts->markLayoutCurrent();
        }
        else m_lastUpload = when;
        saveSettings();
        // A download that worked is what arms the timer - see applyTimer().
        applyTimer();
    }

    setStatus(QString());
    setResult(outcome.success ? outcome.message : outcome.error, !outcome.success);
    emit statusChanged();
    setBusy(false);
}

void AppController::syncFromGoogle()
{
    runSync(Download, false);
}

void AppController::uploadChanges()
{
    runSync(Upload, false);
}

// ============================================================================
// Housekeeping
// ============================================================================
void AppController::loadCredentialsFromFile()
{
    const CredentialsFileResult account = CredentialsFile::load();

    if (account.source.isEmpty()) {
        QString message = tr("No account file found. Copy a file called %1 to the memory card "
                             "or to C:\\Data with two lines in it:")
                              .arg(CredentialsFile::defaultFileName());
        message += QLatin1String("\n\nlogin: you@gmail.com\npassword: your16charapppw");
        setResult(message, true);
        return;
    }

    if (!account.email.isEmpty()) setEmail(account.email);
    if (!account.password.isEmpty()) setPassword(account.password);

    if (!account.problem.isEmpty()) {
        setResult(tr("Read %1, but %2.").arg(account.source, account.problem), true);
        return;
    }
    setResult(tr("Account loaded from %1 (%2).").arg(account.source, account.email), false);
}

void AppController::forgetAccount()
{
    m_timer->stop();

    SyncState state(dataDir());
    state.load();
    state.clear();

    m_email.clear();
    m_password.clear();
    m_lastDownload.clear();
    m_lastUpload.clear();
    m_syncedCount = 0;
    saveSettings();

    emit accountChanged();
    emit statusChanged();
    setResult(tr("Account and sync record forgotten. Contacts already on the phone were kept."),
              false);
}

void AppController::forgetSyncState()
{
    SyncState state(dataDir());
    state.load();
    state.clear();

    m_lastDownload.clear();
    m_lastUpload.clear();
    m_syncedCount = 0;
    saveSettings();
    applyTimer();

    emit statusChanged();
    setResult(tr("Sync record forgotten. The next sync will download everything again."), false);
}
