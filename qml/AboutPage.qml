// GContactsSync - about.
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

            Label { text: qsTr("About"); font.pixelSize: 28 }

            Label {
                width: parent.width
                text: app.aboutText
                font.pixelSize: 16
                wrapMode: Text.WordWrap
            }

            Button {
                width: parent.width
                text: qsTr("Save contact field dump")
                onClicked: app.dumpContactFields()
            }

            Label {
                width: parent.width
                text: app.result
                visible: text.length > 0
                font.pixelSize: 15
                wrapMode: Text.WordWrap
            }

            Item { width: 1; height: platformStyle.paddingLarge }

            Label {
                width: parent.width
                text: qsTr("This code was written with the assistance of Claude Code, Anthropic's agentic coding tool.")
                font.pixelSize: 15
                opacity: 0.6
                wrapMode: Text.WordWrap
            }

            Label {
                width: parent.width
                text: qsTr("GPL-3.0-or-later. Ported from the Windows Phone 8.1 / Windows 10 Mobile version of this app.")
                font.pixelSize: 15
                opacity: 0.6
                wrapMode: Text.WordWrap
            }
        }
    }

    ScrollDecorator { flickableItem: flick }
}
