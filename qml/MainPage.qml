// GContactsSync - the account and the two sync buttons.
// Copyright (C) 2026 - GPL-3.0-or-later, see LICENSE.
import QtQuick 1.1
import com.nokia.symbian 1.1

Page {
    id: page

    /// Hands the controller whatever is currently in the two fields. A field the user is still in
    /// has not lost focus yet, so its value has not been pushed.
    function commitAccount() {
        app.email = emailField.text
        app.password = passwordField.text
    }

    tools: ToolBarLayout {
        ToolButton {
            flat: true
            iconSource: "toolbar-settings"
            enabled: !app.busy
            onClicked: pageStack.push(Qt.resolvedUrl("SettingsPage.qml"))
        }
        // The menu, bottom right, where Symbian puts it.
        ToolButton {
            flat: true
            iconSource: "toolbar-menu"
            onClicked: mainMenu.open()
        }
    }

    Menu {
        id: mainMenu

        MenuLayout {
            MenuItem {
                text: qsTr("Settings")
                onClicked: pageStack.push(Qt.resolvedUrl("SettingsPage.qml"))
            }
            MenuItem {
                text: qsTr("About")
                onClicked: pageStack.push(Qt.resolvedUrl("AboutPage.qml"))
            }
            MenuItem {
                text: qsTr("Exit")
                // Closes the app outright, rather than leaving it in the background the way the
                // red key does. Worth having now that nothing restarts it: with automatic sync on,
                // leaving it running is what keeps the timer alive, so quitting should be a choice
                // the user makes deliberately.
                onClicked: Qt.quit()
            }
        }
    }

    Flickable {
        id: flick
        anchors.fill: parent
        anchors.margins: platformStyle.paddingLarge
        contentWidth: parent.width
        contentHeight: column.height + platformStyle.paddingLarge * 2
        flickableDirection: Flickable.VerticalFlick
        clip: true

        Column {
            id: column
            width: flick.width - platformStyle.paddingLarge * 2
            spacing: platformStyle.paddingMedium

            Label {
                text: qsTr("GContactsSync")
                platformInverted: false
                font.pixelSize: 28
            }
            Label {
                width: parent.width
                text: qsTr("Google Contacts over CardDAV")
                font.pixelSize: 16
                opacity: 0.6
                wrapMode: Text.WordWrap
            }

            // ---- account ----
            //
            // The fields are NOT bound with "onTextChanged: app.x = text". That handler fires on
            // every change to the field - including when QML tears the page down on Exit and the
            // field is cleared - so leaving the app wrote an empty password over the saved one and
            // the account was gone on the next start. The account is pushed to the controller when
            // a field loses focus and again before a sync, and pulled back by accountChanged.
            Label { text: qsTr("Gmail address"); font.pixelSize: 18 }
            TextField {
                id: emailField
                width: parent.width
                inputMethodHints: Qt.ImhEmailCharactersOnly | Qt.ImhNoAutoUppercase
                enabled: !app.busy
                Component.onCompleted: text = app.email
                onActiveFocusChanged: if (!activeFocus) app.email = text
            }

            Label { text: qsTr("App Password (16 characters)"); font.pixelSize: 18 }
            TextField {
                id: passwordField
                width: parent.width
                echoMode: TextInput.Password
                inputMethodHints: Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText
                enabled: !app.busy
                Component.onCompleted: text = app.password
                onActiveFocusChanged: if (!activeFocus) app.password = text
            }

            // Keeps the fields in step when the account changes from somewhere else - loading it
            // from a file, or forgetting it - without writing back while the user is typing.
            Connections {
                target: app
                onAccountChanged: {
                    if (!emailField.activeFocus) emailField.text = app.email
                    if (!passwordField.activeFocus) passwordField.text = app.password
                }
            }
            Label {
                width: parent.width
                text: qsTr("Not your Google password. Create one at myaccount.google.com/apppasswords - it needs 2-Step Verification switched on.")
                font.pixelSize: 15
                opacity: 0.6
                wrapMode: Text.WordWrap
            }

            Button {
                width: parent.width
                text: qsTr("load from file")
                enabled: !app.busy
                onClicked: app.loadCredentialsFromFile()
            }

            // ---- sync ----
            Item { width: 1; height: platformStyle.paddingLarge }

            Button {
                width: parent.width
                text: qsTr("sync from Google")
                enabled: !app.busy
                // Push what is on screen first: a field the user has typed into but not left
                // still has focus, so its value has not reached the controller yet.
                onClicked: { page.commitAccount(); app.syncFromGoogle() }
            }
            Button {
                width: parent.width
                text: qsTr("upload phone changes")
                enabled: !app.busy
                onClicked: { page.commitAccount(); app.uploadChanges() }
            }
            Label {
                width: parent.width
                text: qsTr("Downloading also runs on a timer while this app is open or in the background. Uploading is always manual.")
                font.pixelSize: 15
                opacity: 0.6
                wrapMode: Text.WordWrap
            }

            // ---- progress ----
            Row {
                width: parent.width
                spacing: platformStyle.paddingMedium
                visible: app.busy

                BusyIndicator { running: app.busy; width: 32; height: 32 }
                Label {
                    width: parent.width - 32 - platformStyle.paddingMedium
                    text: app.status
                    font.pixelSize: 16
                    wrapMode: Text.WordWrap
                }
            }
            Button {
                width: parent.width
                text: qsTr("stop")
                visible: app.busy
                onClicked: app.cancel()
            }

            // ---- result ----
            Rectangle {
                width: parent.width
                height: resultLabel.height + platformStyle.paddingLarge * 2
                visible: app.result.length > 0 && !app.busy
                color: app.resultIsError ? "#b71c1c" : "#2e7d32"
                radius: 6

                Label {
                    id: resultLabel
                    anchors.centerIn: parent
                    width: parent.width - platformStyle.paddingLarge * 2
                    text: app.result
                    color: "white"
                    font.pixelSize: 16
                    wrapMode: Text.WordWrap
                }
            }

            // ---- status ----
            Item { width: 1; height: platformStyle.paddingLarge }

            Label {
                width: parent.width
                font.pixelSize: 15
                opacity: 0.7
                wrapMode: Text.WordWrap
                text: app.syncedSummary + "\n"
                      + qsTr("last sync from Google: ") + (app.lastDownload.length ? app.lastDownload : qsTr("never")) + "\n"
                      + qsTr("last upload: ") + (app.lastUpload.length ? app.lastUpload : qsTr("never")) + "\n"
                      + qsTr("automatic sync: ") + (app.autoSyncEnabled ? qsTr("every %1").arg(app.intervalText) : qsTr("off"))
            }
        }
    }

    ScrollDecorator { flickableItem: flick }
}
