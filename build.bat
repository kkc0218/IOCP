@echo off
REM Build script for IOCP Document Server
REM Supports both Visual Studio and MinGW-w64

echo ==========================================
echo   IOCP Document Server Build Script
echo ==========================================
echo.

REM Check if we're in the right directory
if not exist "server_iocp.c" (
    echo ERROR: server_iocp.c not found!
    echo Make sure you're running this script from the project root directory.
    pause
    exit /b 1
)

REM Create build directory
if not exist "build" mkdir build

REM Try to detect compiler
set COMPILER=unknown
where cl >nul 2>&1
if %ERRORLEVEL% == 0 (
    set COMPILER=msvc
    echo Detected: Microsoft Visual C++ Compiler
) else (
    where gcc >nul 2>&1
    if %ERRORLEVEL% == 0 (
        set COMPILER=gcc
        echo Detected: MinGW-w64 GCC Compiler
    )
)

if "%COMPILER%" == "unknown" (
    echo ERROR: No supported compiler found!
    echo Please install either:
    echo   - Visual Studio with C++ tools
    echo   - MinGW-w64
    echo.
    pause
    exit /b 1
)

echo.
echo Building with %COMPILER%...
echo.

if "%COMPILER%" == "msvc" goto :build_msvc
if "%COMPILER%" == "gcc" goto :build_gcc

:build_msvc
echo Building with Visual Studio...
cl /Fe:build\server_iocp.exe server_iocp.c /link ws2_32.lib mswsock.lib
if %ERRORLEVEL% neq 0 (
    echo ERROR: Server build failed!
    pause
    exit /b 1
)

cl /Fe:build\client_iocp.exe client_iocp.c /link ws2_32.lib
if %ERRORLEVEL% neq 0 (
    echo ERROR: Client build failed!
    pause
    exit /b 1
)

REM Build debug versions
echo Building debug versions...
cl /Zi /DEBUG /Fe:build\server_iocp_debug.exe server_iocp.c /link ws2_32.lib mswsock.lib
cl /Zi /DEBUG /Fe:build\client_iocp_debug.exe client_iocp.c /link ws2_32.lib

goto :build_complete

:build_gcc
echo Building with MinGW-w64...
gcc -Wall -Wextra -O2 -o build\server_iocp.exe server_iocp.c -lws2_32 -lmswsock
if %ERRORLEVEL% neq 0 (
    echo ERROR: Server build failed!
    pause
    exit /b 1
)

gcc -Wall -Wextra -O2 -o build\client_iocp.exe client_iocp.c -lws2_32
if %ERRORLEVEL% neq 0 (
    echo ERROR: Client build failed!
    pause
    exit /b 1
)

REM Build debug versions
echo Building debug versions...
gcc -Wall -Wextra -g -O0 -o build\server_iocp_debug.exe server_iocp.c -lws2_32 -lmswsock
gcc -Wall -Wextra -g -O0 -o build\client_iocp_debug.exe client_iocp.c -lws2_32

goto :build_complete

:build_complete
REM Copy config file
copy config.txt build\ >nul 2>&1

echo.
echo ==========================================
echo           BUILD SUCCESSFUL!
echo ==========================================
echo.
echo Built files:
echo   build\server_iocp.exe        - Release server
echo   build\client_iocp.exe        - Release client  
echo   build\server_iocp_debug.exe  - Debug server
echo   build\client_iocp_debug.exe  - Debug client
echo   build\config.txt             - Configuration file
echo.
echo To run the server:
echo   cd build
echo   server_iocp.exe 127.0.0.1 8080
echo.
echo To run the client:
echo   cd build  
echo   client_iocp.exe
echo.
echo For CMake builds, run:
echo   mkdir cmake-build
echo   cd cmake-build
echo   cmake ..
echo   cmake --build .
echo.

REM Ask if user wants to run a quick test
set /p CHOICE="Do you want to run a quick test? (y/n): "
if /i "%CHOICE%" == "y" goto :run_test
if /i "%CHOICE%" == "yes" goto :run_test

echo Build complete!
pause
exit /b 0

:run_test
echo.
echo Starting server on localhost:8080...
echo Press Ctrl+C to stop the server when done testing.
echo.
cd build
start "IOCP Server" server_iocp.exe 127.0.0.1 8080
timeout /t 2 >nul

echo.
echo Server started! You can now run the client in another terminal:
echo   cd build
echo   client_iocp.exe
echo.
pause
exit /b 0
