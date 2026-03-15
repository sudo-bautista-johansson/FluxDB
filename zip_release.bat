@echo off
echo ==============================================
echo  FluxDB Release Bundler (ZIP)
echo ==============================================
echo.

:: 1. Run the distributor first to ensure dist_global is fresh
echo Step 1: Generating Binary Distribution...
call create_binary_dist.bat

echo.
echo Step 2: Zipping for Release...
set ZIP_NAME=fluxdb_v1.0.0_release.zip

:: Use PowerShell to zip the folder
powershell -Command "Compress-Archive -Path dist_global\* -DestinationPath %ZIP_NAME% -Force"

if errorlevel 1 (
    echo [ERROR] Failed to create zip archive.
    pause
    exit /b 1
)

:: Move zip to documentation assets if they exist
if not exist "docs\web\assets" mkdir "docs\web\assets"
move /Y %ZIP_NAME% docs\web\assets\fluxdb_latest.zip

echo.
echo ==============================================
echo Success! Release package created:
echo   Location: docs\web\assets\fluxdb_latest.zip
echo ==============================================
pause
