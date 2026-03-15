@echo off
echo ==============================================
echo  VeldraDB C++ Engine System Installer (Win64)
echo ==============================================
echo.
echo Compiling VeldraDB Engine...
g++ -shared -o veldra.dll core\query\*.cpp core\ecs\*.cpp core\storage\*.cpp core\spatial\*.cpp -I. -std=c++17

if errorlevel 1 (
    echo [ERROR] Compilation failed. Ensure g++ and Windows SDKs are installed.
    pause
    exit /b 1
)

echo.
echo [1/3] Copying veldra.dll to System32...
copy /Y veldra.dll C:\Windows\System32\veldra.dll

echo [2/3] Creating Global Include Headers...
if not exist "C:\VeldraDB\include" mkdir "C:\VeldraDB\include"
copy /Y core\query\veldra_c_api.h C:\VeldraDB\include\veldra_c_api.h

echo [3/3] Setting User Environment Variable VELDRA_PATH
setx VELDRA_PATH "C:\VeldraDB"

echo.
echo ==============================================
echo Success! VeldraDB Engine is now installed globally.
echo You can now use the engine in any project using:
echo   Python: 'pip install .' in bindings/python/
echo   C++: Include #include ^<veldra_c_api.h^> and link against C:\Windows\System32\veldra.dll
echo ==============================================
pause
