# GContactsSync - desktop harness for the sync core.
# Copyright (C) 2026 - GPL-3.0-or-later, see LICENSE.
#
# Build with the Desktop Qt 4.7.4 MinGW kit (build-desktop.cmd at the repo root).
#
#   gcontactssync-cli                 the offline checks - vCard parsing, patching, the
#                                     fingerprint, the account file. No network, no account.
#   gcontactssync-cli --live <dir>    a real sync against Google, with an in-memory phonebook
#                                     standing in for the phone. Reads the account from
#                                     gcontactssync.txt (see docs/gcontactssync.txt.sample).
#   gcontactssync-cli --live <dir> --upload
#                                     as above, then edits one contact in the in-memory phonebook
#                                     and uploads it, to exercise the patch path end to end.
#
# The point of this existing: everything except the QtContacts bridge can be proven on a PC,
# against the real server, before a .sis is ever installed.

TEMPLATE = app
TARGET = gcontactssync-cli
CONFIG += console
CONFIG -= app_bundle
QT -= gui

include(../../core.pri)

SOURCES += main.cpp
