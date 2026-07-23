@echo off
setlocal

:: --- Locate MSYS2 MinGW64 toolchain ---
:: We look in the local external_source folder first
set "MINGW_BIN=%~dp0external_source\msys64\mingw64\bin"

if not exist "%MINGW_BIN%\g++.exe" (
    echo ERROR: MinGW-w64 not found at %MINGW_BIN%
    echo.
    echo Please make sure MSYS2 is placed in the external_source\msys64 folder,
    echo and the mingw-w64-x86_64-gcc and mingw-w64-x86_64-make packages are installed.
    echo.
    pause
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

echo.
echo Build successful. Standalone binary: output\OmenReplace.exe
pause
