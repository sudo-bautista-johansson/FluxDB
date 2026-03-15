@echo off
echo ==============================================
echo  FluxDB Master Binary Distributon Creator
echo ==============================================
echo.

set BUILD_DIR=dist_global
if exist "%BUILD_DIR%" rmdir /s /q "%BUILD_DIR%"
mkdir "%BUILD_DIR%"

:: 1. Core Native Binaries
echo Packaging Native Binaries...
mkdir "%BUILD_DIR%\native\win64"
if exist "flux.dll" copy "flux.dll" "%BUILD_DIR%\native\win64\"
mkdir "%BUILD_DIR%\native\include"
copy "core\headers\flux_c_api.h" "%BUILD_DIR%\native\include\"
copy "core\headers\fluxdb.h" "%BUILD_DIR%\native\include\"

:: 2. Python Wheel
echo Packaging Python (WHL)...
mkdir "%BUILD_DIR%\python"
cd bindings\python
call publish_pypi.bat
cd ..\..
copy bindings\python\dist\*.whl "%BUILD_DIR%\python\"

:: 3. Unity Package
echo Packaging Unity (UPM)...
mkdir "%BUILD_DIR%\unity"
cd upm
call package_unity.bat
cd ..
xcopy /E /I "upm\dist_upm" "%BUILD_DIR%\unity"

:: 4. Unreal Plugin
echo Packaging Unreal (Binary Plugin)...
mkdir "%BUILD_DIR%\unreal"
cd unreal_plugin
call dist_unreal.bat
cd ..
xcopy /E /I "unreal_plugin\dist_unreal" "%BUILD_DIR%\unreal"

echo.
echo ==============================================
echo Global Binary Distribution Complete!
echo Final output is in: %BUILD_DIR%
echo ==============================================
exit /b 0
