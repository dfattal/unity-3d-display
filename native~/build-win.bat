@echo off
setlocal

echo === DisplayXR Unity Plugin — Windows MSVC Build ===

cd /d "%~dp0"

if exist build (
    echo Cleaning previous build...
    rmdir /s /q build
)

echo Configuring (MSVC x64)...
cmake -S . -B build -A x64 -DCMAKE_BUILD_TYPE=Release
if errorlevel 1 (
    echo ERROR: CMake configure failed.
    exit /b 1
)

echo Building...
cmake --build build --config Release
if errorlevel 1 (
    echo ERROR: Build failed.
    exit /b 1
)

echo.
echo === Build complete ===
echo DLL: ..\Runtime\Plugins\Windows\x64\displayxr_unity.dll
dir "..\Runtime\Plugins\Windows\x64\displayxr_unity.dll"
