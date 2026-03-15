@echo off
echo ==============================================
echo  FluxDB Unity Package (UPM) Distributor
echo ==============================================
echo.

:: Ensure we are in the right directory
cd %~dp0

set DIST_DIR=dist_upm
set PKG_DIR=.

:: 1. Cleanup
if exist "%DIST_DIR%" rmdir /s /q "%DIST_DIR%"
mkdir "%DIST_DIR%"

:: 2. Copy UPM folder (only Runtime and package.json)
echo Packaging Unity...
mkdir "%DIST_DIR%\Runtime"
copy "package.json" "%DIST_DIR%\"
copy "Runtime\FluxDB*" "%DIST_DIR%\Runtime\"

:: 3. Note: In a real CI, we would use 'npm pack' here to create a .tgz
echo.
echo ==============================================
echo Success! Unity distribution ready in '%DIST_DIR%'.
echo This folder contains NO C++ SOURCE CODE.
echo You can zip this folder or upload it to a Git/UPM registry.
echo ==============================================
exit /b 0
