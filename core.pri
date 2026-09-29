# GContactsSync - a Google Contacts CardDAV client for Symbian^3 / Anna / Belle.
# Copyright (C) 2026 - GPL-3.0-or-later, see LICENSE.
#
# The CardDAV / vCard / sync core, shared by the phone app (GContactsSync.pro) and the desktop
# harness (tools/gcontactssync-cli). Pure QtCore + QtNetwork: no UI, no Qt Mobility, nothing
# Symbian-specific, which is what lets the harness build and run the whole sync on a PC.
#
# The phonebook is reached through IContactStore (src/core/contactstore.h); the Symbian
# implementation over Qt Mobility's QtContacts lives in src/app and is not part of this file.

QT += core network

INCLUDEPATH += $$PWD/src/core

HEADERS += \
    $$PWD/src/core/vcard.h \
    $$PWD/src/core/contacthash.h \
    $$PWD/src/core/contactstore.h \
    $$PWD/src/core/credentialsfile.h \
    $$PWD/src/core/carddavclient.h \
    $$PWD/src/core/syncstate.h \
    $$PWD/src/core/syncengine.h

SOURCES += \
    $$PWD/src/core/vcard.cpp \
    $$PWD/src/core/contacthash.cpp \
    $$PWD/src/core/credentialsfile.cpp \
    $$PWD/src/core/carddavclient.cpp \
    $$PWD/src/core/syncstate.cpp \
    $$PWD/src/core/syncengine.cpp
