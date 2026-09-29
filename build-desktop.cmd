@echo off
rem Builds and runs the desktop harness (tools/gcontactssync-cli) with the Desktop Qt 4.7.4 MinGW
rem kit. Everything in src/core is plain QtCore + QtNetwork, so the vCard handling, the patcher,
rem the fingerprint, the account file and - with --live - the whole sync against Google can be
rem checked on a PC before a .sis is ever installed.
rem
rem   build-desktop.cmd                   build, then run the offline checks
rem   build-desktop.cmd --live            build, then a real sync against Google (needs an
rem                                       account file - see docs/gcontactssync.txt.sample)
rem   build-desktop.cmd --live --upload   as above, then edit one contact and upload it
rem   build-desktop.cmd clean             remove the build output
setlocal

set "QTDIR=D:\QtSDK\Desktop\Qt\4.7.4\mingw"
set "MINGW=D:\QtSDK\mingw"
if not exist "%QTDIR%\bin\qmake.exe" echo ERROR: Desktop Qt 4.7.4 MinGW kit not found at %QTDIR% & exit /b 1

set "PATH=%QTDIR%\bin;%MINGW%\bin;%PATH%"
set "BUILD=%~dp0build-cli"

if /i "%1"=="clean" (
    if exist "%BUILD%" rd /s /q "%BUILD%"
    echo Clean.
    exit /b 0
)

if not exist "%BUILD%" mkdir "%BUILD%"
cd /d "%BUILD%"

qmake "%~dp0tools\gcontactssync-cli\gcontactssync-cli.pro" -spec win32-g++ CONFIG+=release
if errorlevel 1 exit /b 1

mingw32-make -s release
if errorlevel 1 (
    echo ================ BUILD FAILED ================
    exit /b 1
)

echo === built %BUILD%\release\gcontactssync-cli.exe
echo.
"%BUILD%\release\gcontactssync-cli.exe" %*
exit /b %errorlevel%
