@echo off
setlocal

if not exist build mkdir build

echo Configuring with CMake...
cmake -G "MinGW Makefiles" -B build .
if %errorlevel% neq 0 (
    echo Configuration failed!
    exit /b %errorlevel%
)

echo Building...
cmake --build build
if %errorlevel% neq 0 (
    echo Build failed!
    exit /b %errorlevel%
)

echo Build successful. Binary is in the 'output' directory.
pause
