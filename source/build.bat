@echo off
REM ================================================================
REM Build script for Taskbar Tray Icon Resizer
REM ================================================================
REM
REM Requirements:
REM   - Visual Studio 2022+ with "Desktop development with C++" workload
REM   - Windows SDK (included with VS)
REM
REM Build steps:
REM   1. Open "x64 Native Tools Command Prompt for VS 2022"
REM   2. cd to this directory
REM   3. Run: build.bat
REM ================================================================

setlocal enabledelayedexpansion

echo.
echo === Building Taskbar Tray Icon Resizer ===
echo.

REM Check if we're in a VS dev environment
where cl.exe >nul 2>nul
if %ERRORLEVEL% NEQ 0 (
    echo ERROR: cl.exe not found in PATH.
    echo.
    echo Open a "x64 Native Tools Command Prompt for VS 2022" first.
    echo You can also try:
    echo   "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
    echo.
    pause
    exit /b 1
)

REM Check windowsapp.lib availability
where /R "%WindowsSdkDir%" windowsapp.lib >nul 2>nul
if %ERRORLEVEL% NEQ 0 (
    echo WARNING: windowsapp.lib not found. Ensure Windows SDK is installed.
)

REM Build the DLL (the worker that gets injected into explorer.exe)
echo [1/2] Building tray_resizer_dll.dll...
cl /EHsc /MD /LD /std:c++20 /Zc:preprocessor /nologo ^
    tray_resizer_dll.cpp ^
    /Fe:tray_resizer_dll.dll ^
    /I"%WindowsSdkDir%Include\%WindowsSDKVersion%\cppwinrt" ^
    /link windowsapp.lib ole32.lib oleaut32.lib runtimeobject.lib

if %ERRORLEVEL% NEQ 0 (
    echo ERROR: DLL build failed!
    pause
    exit /b 1
)
echo DLL built successfully.
echo.

REM Build the C# injector
echo [2/2] Building tray_resizer.exe (C# injector)...

REM Find csc.exe
set CSC="%SystemRoot%\Microsoft.NET\Framework64\v4.0.30319\csc.exe"
if not exist %CSC% (
    set CSC="%SystemRoot%\Microsoft.NET\Framework\v4.0.30319\csc.exe"
)

%CSC% /platform:x64 /nologo /out:tray_resizer.exe tray_resizer.cs

if %ERRORLEVEL% NEQ 0 (
    echo ERROR: Injector build failed!
    pause
    exit /b 1
)
echo Injector built successfully.
echo.

echo ================================================================
echo BUILD COMPLETE!
echo ================================================================
echo.
echo To use:
echo   1. Run tray_resizer.exe AS ADMINISTRATOR
echo   2. Tray icons will be resized to 24px
echo.
echo To customize icon size, edit Config::kIconWidth in
echo tray_resizer_dll.cpp and rebuild.
echo.

pause
