@echo off
rem Builds the phone package from the command line (Qt Creator does the same through its Symbian
rem kit). Qt for Symbian supports in-source builds only, so this runs in the project directory; the
rem generated files are listed in .gitignore.
rem
rem   build-symbian.cmd          release ARMv5 build, then the self-signed package:
rem                                GContactsSync_<ver>.sis   installs on Belle and Symbian^3 / Anna
rem                              (bumps the patch version first, because Symbian refuses to replace
rem                              an installed app with an equal version)
rem   build-symbian.cmd nobump   the same without bumping, for a rebuild you are not deploying
rem   build-symbian.cmd clean    removes the build output
setlocal
call D:\QtSDK\Symbian\SDKs\SymbianSR1Qt474\env.bat
cd /d %~dp0

if "%1"=="clean" (
    call sbs -c arm.v5.urel.gcce4_4_1 clean
    goto :eof
)

if /i not "%1"=="nobump" (
    rem Bump first: the version in GContactsSync.pro is then the one being built, and each build is
    rem higher than the last.
    python bump-version.py
    if errorlevel 1 exit /b 1
)
for /f "delims=" %%v in ('python bump-version.py --print') do set VER=%%v
echo === building GContactsSync %VER%

qmake GContactsSync.pro -spec symbian-sbsv2 CONFIG+=release
if errorlevel 1 exit /b 1
call sbs -c arm.v5.urel.gcce4_4_1
if errorlevel 1 exit /b 1

rem One self-signed package, which installs on Belle and on Symbian^3 / Anna.
call createpackage.bat GContactsSync_template.pkg release-armv5
if errorlevel 1 exit /b 1

rem The package that is kept carries the version in its name; the unsigned intermediate goes.
if exist GContactsSync.sis (
  move /y GContactsSync.sis GContactsSync_%VER%.sis >nul
  echo built GContactsSync_%VER%.sis
)
del /q GContactsSync_unsigned.sis 2>nul
