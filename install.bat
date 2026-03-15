@echo off
echo ==============================================
echo  FluxDB C++ Engine System Installer (Win64)
echo ==============================================
echo.
echo Compiling FluxDB Engine...
echo [DEBUG] Checking for file locks on flux.dll...
taskkill /F /IM veldra.exe /T 2>nul
taskkill /F /IM veldra_cli.exe /T 2>nul
taskkill /F /IM flux_inspector.exe /T 2>nul

if exist flux.dll (
    del /f /q flux.dll
    if exist flux.dll (
        echo [ERROR] flux.dll is LOCKED by another process.
        echo Please close all programs using FluxDB and try again.
        pause
        exit /b 1
    )
)

g++ -shared -o flux.dll ^
    -x c build\_deps\lua-src\onelua.c -DMAKE_LIB ^
    -x c++ ^
    core\query\*.cpp ^
    core\ecs\*.cpp ^
    core\storage\*.cpp ^
    core\spatial\*.cpp ^
    core\network\*.cpp ^
    core\types\*.cpp ^
    -I. -Icore\headers -Ibuild\_deps\lua-src ^
    -std=c++17 -lws2_32 -lwinmm -static-libgcc -static-libstdc++

if errorlevel 1 (
    echo [ERROR] Compilation failed. Ensure g++ and Windows SDKs are installed.
    pause
    exit /b 1
)

echo.
echo [1/3] Copying flux.dll to System32...
copy /Y flux.dll C:\Windows\System32\flux.dll

echo [2/3] Creating Global Include Headers...
if not exist "C:\FluxDB\include" mkdir "C:\FluxDB\include"
copy /Y core\headers\flux_c_api.h C:\FluxDB\include\flux_c_api.h

echo [3/3] Setting User Environment Variable FLUX_PATH
setx FLUX_PATH "C:\FluxDB"

echo.
echo ==============================================
echo Success! FluxDB Engine is now installed globally.
echo You can now use the engine in any project using:
echo   Python: 'pip install .' in bindings/python/
echo   C++: Include #include ^<flux_c_api.h^> and link against C:\Windows\System32\flux.dll
echo ==============================================
pause
