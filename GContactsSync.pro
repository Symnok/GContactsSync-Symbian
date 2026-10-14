# GContactsSync - a Google Contacts CardDAV client for Symbian^3 / Anna / Belle.
# Copyright (C) 2026 - GPL-3.0-or-later, see LICENSE.
#
# Build for the phone with the "Qt 4.7.4 for Symbian Anna/Belle" (SymbianSR1Qt474) kit, through
# build-symbian.cmd. The sync core (core.pri) is shared with the desktop harness in
# tools/gcontactssync-cli, which is where it is tested - see docs/TESTING.md.

TEMPLATE = app
TARGET = GContactsSync
VERSION = 1.0.17

QT += core gui network declarative

include(core.pri)

INCLUDEPATH += src/app

HEADERS += \
    src/app/appcontroller.h \
    src/app/autostart.h \
    src/app/phonecontacts.h

SOURCES += \
    src/main.cpp \
    src/app/appcontroller.cpp \
    src/app/autostart.cpp \
    src/app/phonecontacts.cpp

RESOURCES += resources.qrc

# The version reaches the About page as an unquoted macro (stringified in code), which survives
# every generator's quoting rules.
DEFINES += APP_VERSION=$$VERSION

OTHER_FILES += qml/*.qml README.md docs/gcontactssync.txt.sample GContactsSync_startup.rss

symbian {
    # Unprotected UID range (0xE...): installs self-signed, with no Symbian Signed submission.
    TARGET.UID3 = 0xE7C1A5D2

    # The phonebook is reached through native CContactDatabase, not Qt Mobility - see
    # src/app/phonecontacts.h. QContactManager was tried first and did not work on the device:
    # most saves were rejected and none of the accepted ones appeared in Contacts. SymGContacts,
    # which syncs Google contacts to these same phones, avoids it for the same reason.
    LIBS += -lcntmodel

    # NetworkServices for the CardDAV requests; ReadUserData and WriteUserData for the phonebook.
    # All three are grantable to a self-signed package - nothing here needs a Symbian Signed
    # capability, which is the reason the sync is built on QtContacts rather than the native
    # Contacts Model.
    TARGET.CAPABILITY += NetworkServices ReadUserData WriteUserData

    # A few hundred contacts, each with a cached vCard and possibly a photo, plus the XML of a
    # twenty-card multiget response: a roomy heap, and the default 8 KB stack is not enough for
    # the XML reader.
    TARGET.EPOCHEAPSIZE = 0x020000 0x4000000
    TARGET.EPOCSTACKSIZE = 0x14000

    ICON = icon.svg

    # The startup list entry, behind the Autostart setting. It is compiled here and shipped in
    # \resource\apps\ - NOT installed into the Startup List Manager's import directory. The app
    # copies it there itself when the setting is switched on; see src/app/autostart.cpp.
    #
    # Note what is deliberately absent: AllFiles. Declaring it would let the app write into that
    # directory on any phone, but a self-signed package carrying AllFiles will not install on a
    # stock one at all. So the write is simply attempted, and fails cleanly where it is forbidden.
    #
    # SOURCEPATH first: START RESOURCE resolves against whatever SOURCEPATH the generated .mmp last
    # set, which by this point is src/core.
    MMP_RULES += "SOURCEPATH ." "START RESOURCE GContactsSync_startup.rss" "TARGETPATH /resource/apps" "END"

    startupitem.sources = $$EPOCROOT/epoc32/data/z/resource/apps/GContactsSync_startup.rsc
    startupitem.path = /resource/apps
    DEPLOYMENT += startupitem


    # Qt Quick Components for Symbian (built into Belle; Anna gets them through the Smart
    # Installer wrapper).
    CONFIG += qt-components
    DEPLOYMENT.installer_header = 0x2002CCCF

    vendorinfo = \
        "%{\"GContactsSync\"}" \
        ":\"GContactsSync\""
    my_deployment.pkg_prerules = vendorinfo
    DEPLOYMENT += my_deployment
}
