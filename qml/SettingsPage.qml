// GContactsSync - whether the timed download runs, how often, and the two ways to start over.
// Copyright (C) 2026 - GPL-3.0-or-later, see LICENSE.
import QtQuick 1.1
import com.nokia.symbian 1.1

Page {
    id: page

    tools: ToolBarLayout {
        ToolButton {
            flat: true
            iconSource: "toolbar-back"
            onClicked: pageStack.pop()
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

            Label { text: qsTr("Settings"); font.pixelSize: 28 }

            // ---- automatic download ----
            Label { text: qsTr("sync from Google"); font.pixelSize: 20; opacity: 0.7 }

            Row {
                width: parent.width
                spacing: platformStyle.paddingMedium

                Label {
                    width: parent.width - autoSwitch.width - platformStyle.paddingMedium
                    text: qsTr("download automatically")
                    font.pixelSize: 18
                    wrapMode: Text.WordWrap
                    anchors.verticalCenter: parent.verticalCenter
                }
                Switch {
                    id: autoSwitch
                    checked: app.autoSyncEnabled
                    onCheckedChanged: app.autoSyncEnabled = checked
                }
            }

            Label { text: qsTr("check every"); font.pixelSize: 18 }

            // A Button rather than a SelectionListItem. SelectionListItem draws its own title in
            // the platform list-item colours, which came out grey on grey on the device and was
            // not readable at all. A Button is the control used everywhere else on this page, so
            // it is styled consistently and its label is legible.
            Button {
                id: intervalButton
                width: parent.width
                text: app.intervalText
                onClicked: intervalPicker.open()
            }

            ListModel {
                id: intervalModel
                ListElement { name: "15 minutes"; minutes: 15 }
                ListElement { name: "30 minutes"; minutes: 30 }
                ListElement { name: "1 hour"; minutes: 60 }
                ListElement { name: "2 hours"; minutes: 120 }
                ListElement { name: "4 hours"; minutes: 240 }
                ListElement { name: "8 hours"; minutes: 480 }
            }

            // ---- autostart ----
            Item { width: 1; height: platformStyle.paddingLarge }
            Label { text: qsTr("start with the phone"); font.pixelSize: 20; opacity: 0.7 }

            Row {
                width: parent.width
                spacing: platformStyle.paddingMedium

                Label {
                    width: parent.width - autostartSwitch.width - platformStyle.paddingMedium
                    text: qsTr("autostart")
                    font.pixelSize: 18
                    wrapMode: Text.WordWrap
                    anchors.verticalCenter: parent.verticalCenter
                }
                Switch {
                    id: autostartSwitch
                    // Bound to the real state, which is whether the entry exists on the phone. If
                    // the write is refused the property does not change, and this snaps back.
                    checked: app.autostartEnabled
                    onCheckedChanged: if (checked !== app.autostartEnabled)
                                          app.autostartEnabled = checked
                }
            }

            Label {
                width: parent.width
                text: qsTr("Launches GContactsSync when the phone starts, so the sync timer is running without you opening it. This needs a phone whose platform security has been patched; on a stock phone the switch will refuse to stay on, and it says so.")
                font.pixelSize: 15
                opacity: 0.6
                wrapMode: Text.WordWrap
            }

            Label {
                width: parent.width
                visible: app.result.length > 0
                text: app.result
                color: app.resultIsError ? "#FF6B6B" : "#7BD88F"
                font.pixelSize: 15
                wrapMode: Text.WordWrap
            }

            Label {
                width: parent.width
                text: qsTr("The timer runs inside this app: while it is open, and while it sits in the background after the red key. Symbian has no way to sync with the app fully closed. Fifteen minutes is the shortest interval, so this behaves like the Windows Phone version.")
                font.pixelSize: 15
                opacity: 0.6
                wrapMode: Text.WordWrap
            }

            // ---- upload ----
            Item { width: 1; height: platformStyle.paddingLarge }
            Label { text: qsTr("upload to Google"); font.pixelSize: 20; opacity: 0.7 }
            Label {
                width: parent.width
                text: qsTr("Uploading is always manual, from the button on the main page. A timer cannot ask you anything, and an edit made by accident should not reach Google unattended.")
                font.pixelSize: 15
                opacity: 0.6
                wrapMode: Text.WordWrap
            }

            // ---- starting over ----
            Item { width: 1; height: platformStyle.paddingLarge }
            Label { text: qsTr("start over"); font.pixelSize: 20; opacity: 0.7 }

            Button {
                width: parent.width
                text: qsTr("forget sync record")
                enabled: !app.busy
                onClicked: resetDialog.open()
            }
            Label {
                width: parent.width
                text: qsTr("Makes the next sync a full one. The account stays, and contacts on the phone are kept.")
                font.pixelSize: 15
                opacity: 0.6
                wrapMode: Text.WordWrap
            }

            Button {
                width: parent.width
                text: qsTr("forget account")
                enabled: !app.busy
                onClicked: forgetDialog.open()
            }
            Label {
                width: parent.width
                text: qsTr("Forgets the address, the App Password and the sync record. Contacts already on the phone are left alone.")
                font.pixelSize: 15
                opacity: 0.6
                wrapMode: Text.WordWrap
            }
        }
    }

    ScrollDecorator { flickableItem: flick }

    // The interval picker, built by hand rather than with SelectionDialog.
    //
    // SelectionDialog styles its own list rows from the platform theme, and on the device that
    // came out grey text on a grey background - unreadable, and with no property to override it.
    // SelectionListItem had the same problem before it. So this depends on no theme colours at
    // all: every colour here is stated outright, and the rows are Buttons, which are the one
    // control on this page known to render correctly on the phone.
    Item {
        id: intervalPicker
        anchors.fill: parent
        visible: false
        z: 100

        function open() { visible = true }
        function close() { visible = false }

        // Dims the page behind, and closes when tapped outside the panel.
        Rectangle {
            anchors.fill: parent
            color: "#000000"
            opacity: 0.75
            MouseArea { anchors.fill: parent; onClicked: intervalPicker.close() }
        }

        Rectangle {
            anchors.centerIn: parent
            width: Math.min(parent.width - 48, 340)
            height: pickerColumn.height + 36
            color: "#1C1C1C"
            border.color: "#5A5A5A"
            border.width: 1
            radius: 8

            Column {
                id: pickerColumn
                anchors.centerIn: parent
                width: parent.width - 36
                spacing: 10

                Label {
                    width: parent.width
                    text: qsTr("Check every")
                    color: "#FFFFFF"
                    font.pixelSize: 20
                }

                Repeater {
                    model: intervalModel

                    Button {
                        width: pickerColumn.width
                        // A tick marks the one in use, since a Button gives no selected state.
                        text: (minutes === app.intervalMinutes ? "•  " : "") + name
                        onClicked: {
                            app.intervalMinutes = minutes
                            intervalPicker.close()
                        }
                    }
                }

                Button {
                    width: pickerColumn.width
                    text: qsTr("cancel")
                    onClicked: intervalPicker.close()
                }
            }
        }
    }

    QueryDialog {
        id: resetDialog
        titleText: qsTr("Forget sync record?")
        message: qsTr("The next sync will download every contact from Google again. Contacts on the phone are kept.")
        acceptButtonText: qsTr("Forget")
        rejectButtonText: qsTr("Cancel")
        onAccepted: { app.forgetSyncState(); pageStack.pop() }
    }

    QueryDialog {
        id: forgetDialog
        titleText: qsTr("Forget account?")
        message: qsTr("The address, the App Password and the sync record are all forgotten. Contacts already on the phone are left alone.")
        acceptButtonText: qsTr("Forget")
        rejectButtonText: qsTr("Cancel")
        onAccepted: { app.forgetAccount(); pageStack.pop() }
    }
}
