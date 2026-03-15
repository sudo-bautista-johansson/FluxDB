@echo off
echo ==============================================
echo  FluxDB Unreal Engine Plugin Distributor
echo ==============================================
echo.

:: This script creates a binary-only version of the plugin
cd %~dp0

set DIST_DIR=dist_unreal
set PLUGIN_DIR=flux_plugin

:: 1. Cleanup
if exist "%DIST_DIR%" rmdir /s /q "%DIST_DIR%"
mkdir "%DIST_DIR%"

:: 2. Copy Plugin descriptor
copy "FluxDB.uplugin" "%DIST_DIR%\"

:: 3. Copy Source (Public headers only, NO .cpp)
echo Copying Public Headers...
mkdir "%DIST_DIR%\Source\FluxDB\Public"
copy "Source\FluxDB\Public\FluxDB*.h" "%DIST_DIR%\Source\FluxDB\Public\"
copy "Source\FluxDB\FluxDB.Build.cs" "%DIST_DIR%\Source\FluxDB\"

:: 4. Copy Binaries if they exist
if exist "Binaries" (
    echo Copying Compiled Binaries...
    xcopy /E /I "Binaries" "%DIST_DIR%\Binaries"
)

echo.
echo ==============================================
echo Success! Binary-only Unreal Plugin ready in '%DIST_DIR%'.
echo Internal logic (.cpp) has been excluded.
echo ==============================================
exit /b 0
