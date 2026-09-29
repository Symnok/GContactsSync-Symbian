// GContactsSync - a Google Contacts CardDAV client for Symbian^3 / Anna / Belle.
// Copyright (C) 2026 - GPL-3.0-or-later, see LICENSE.
//
// Everything the QML sees. It owns the account, the settings, the phonebook bridge and the timer,
// and it turns a tap into a SyncEngine call; it contains no sync logic of its own.
//
// The timer is how the periodic download happens. Symbian has no equivalent of the WinRT
// background task the Windows Phone version registers, so the download runs on a QTimer inside
// this process: while the app is open, and while it sits in the background after the red key,
// which on Symbian is where an app normally lives. Fifteen minutes is the floor, matching the
// other versions of this app - not because the platform insists, but so the three behave alike.
#ifndef APPCONTROLLER_H
#define APPCONTROLLER_H

#include <QtCore/QObject>
#include <QtCore/QString>

class PhoneContacts;
class QTimer;
class SyncState;

class AppController : public QObject
{
    Q_OBJECT

    Q_PROPERTY(QString email READ email WRITE setEmail NOTIFY accountChanged)
    Q_PROPERTY(QString password READ password WRITE setPassword NOTIFY accountChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
    Q_PROPERTY(QString result READ result NOTIFY resultChanged)
    Q_PROPERTY(bool resultIsError READ resultIsError NOTIFY resultChanged)
    Q_PROPERTY(bool autoSyncEnabled READ autoSyncEnabled WRITE setAutoSyncEnabled NOTIFY settingsChanged)
    Q_PROPERTY(int intervalMinutes READ intervalMinutes WRITE setIntervalMinutes NOTIFY settingsChanged)
    Q_PROPERTY(QString intervalText READ intervalText NOTIFY settingsChanged)
    Q_PROPERTY(bool autostartEnabled READ autostartEnabled WRITE setAutostartEnabled NOTIFY settingsChanged)
    Q_PROPERTY(QString lastDownload READ lastDownload NOTIFY statusChanged)
    Q_PROPERTY(QString lastUpload READ lastUpload NOTIFY statusChanged)
    Q_PROPERTY(QString syncedSummary READ syncedSummary NOTIFY statusChanged)
    Q_PROPERTY(QString version READ version CONSTANT)
    Q_PROPERTY(QString aboutText READ aboutText CONSTANT)

public:
    explicit AppController(QObject *parent = 0);
    ~AppController();

    /// The smallest interval the UI may offer, so all three ports of this app behave alike.
    static const int MinimumIntervalMinutes = 15;

    QString email() const { return m_email; }
    void setEmail(const QString &value);
    QString password() const { return m_password; }
    void setPassword(const QString &value);

    bool busy() const { return m_busy; }
    QString status() const { return m_status; }
    QString result() const { return m_result; }
    bool resultIsError() const { return m_resultIsError; }

    bool autoSyncEnabled() const { return m_autoSyncEnabled; }
    void setAutoSyncEnabled(bool value);
    int intervalMinutes() const { return m_intervalMinutes; }
    void setIntervalMinutes(int value);

    /// The interval in words - "30 minutes", "1 hour", "4 hours" - so the UI never has to show
    /// 480 minutes to mean eight hours.
    QString intervalText() const;

    /// Whether the phone will launch this app at boot. Read from the file system rather than from
    /// a setting, so it can never claim something the phone will not actually do.
    bool autostartEnabled() const;
    void setAutostartEnabled(bool value);

    QString lastDownload() const { return m_lastDownload; }
    QString lastUpload() const { return m_lastUpload; }
    QString syncedSummary() const;
    QString version() const;
    QString aboutText() const;

public slots:
    /// Google -> phone. Full the first time, then only what changed.
    void syncFromGoogle();

    /// Phone -> Google, manual only.
    void uploadChanges();

    /// Fills the account in from a text file on the memory card or in phone memory.
    void loadCredentialsFromFile();

    /// Forgets the account and everything remembered about previous syncs. Contacts already on
    /// the phone are left alone.
    void forgetAccount();

    /// Makes the next sync a full one. Keeps the account and the contacts.
    void forgetSyncState();

    /// Asks the sync in progress to stop after the current request.
    void cancel();

signals:
    void accountChanged();
    void busyChanged();
    void statusChanged();
    void resultChanged();
    void settingsChanged();

private slots:
    void onTimer();
    void onProgress(const QString &message);

private:
    enum Direction { Download, Upload };

    /// The one entry point for both directions: guards against re-entry, builds the client and the
    /// state, runs the engine and reports.
    void runSync(Direction direction, bool fromTimer);

    bool haveUsableAccount(QString *why) const;
    void loadSettings();
    void saveSettings() const;
    void applyTimer();
    void setBusy(bool value);
    void setStatus(const QString &value);
    void setResult(const QString &value, bool isError);
    QString dataDir() const;
    static void installTrustBundle();

    PhoneContacts *m_contacts;
    QTimer *m_timer;

    QString m_email;
    QString m_password;
    bool m_autoSyncEnabled;
    int m_intervalMinutes;
    QString m_lastDownload;
    QString m_lastUpload;

    bool m_busy;
    bool m_cancelRequested;
    QString m_status;
    QString m_result;
    bool m_resultIsError;
    int m_syncedCount;

    /// The in-flight client, so cancel() can reach it.
    class CardDavClient *m_client;
};

#endif // APPCONTROLLER_H
