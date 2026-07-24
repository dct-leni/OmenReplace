@echo off
setlocal

:: --- Locate MSYS2 MinGW64 toolchain ---
set "MINGW_BIN=%~dp0external_source\msys64\mingw64\bin"

if not exist "%MINGW_BIN%\g++.exe" (
    echo ERROR: MinGW-w64 not found at %MINGW_BIN%
    exit /b 1
)

set "PATH=%MINGW_BIN%;%PATH%"

if exist build rmdir /s /q build
mkdir build

echo Configuring with CMake...
cmake -G "MinGW Makefiles" ^
  -DCMAKE_MAKE_PROGRAM="%MINGW_BIN%\mingw32-make.exe" ^
  -DCMAKE_CXX_COMPILER="%MINGW_BIN%\g++.exe" ^
  -B build
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

echo Build successful. Binary generated in output directory.
exit /b 0
