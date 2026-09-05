@echo off
setlocal EnableExtensions

set "ROOT=%~dp0"
if "%ROOT:~-1%"=="\" set "ROOT=%ROOT:~0,-1%"

:: Locate MSVC vcvars64.bat (x86 and x64 install roots)
set "VCTOOLS=%ProgramFiles%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build"
if exist "%VCTOOLS%\vcvars64.bat" goto :found_vc
set "VCTOOLS=%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build"
if exist "%VCTOOLS%\vcvars64.bat" goto :found_vc
set "VCTOOLS=%ProgramFiles%\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build"
if exist "%VCTOOLS%\vcvars64.bat" goto :found_vc
set "VCTOOLS=%ProgramFiles%\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build"
if exist "%VCTOOLS%\vcvars64.bat" goto :found_vc
set "VCTOOLS=%ProgramFiles%\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build"
if exist "%VCTOOLS%\vcvars64.bat" goto :found_vc

echo ERROR: vcvars64.bat not found. Run requirements.bat first.
exit /b 1

:found_vc
echo MSVC found: %VCTOOLS%

:: Clean stale build
if exist "%ROOT%\build" rmdir /s /q "%ROOT%\build"

:: Activate MSVC environment + configure + build
call "%VCTOOLS%\vcvars64.bat" >nul 2>&1
if errorlevel 1 (
  echo vcvars64.bat failed.
  exit /b 1
)

echo.
echo Configuring with CMake (Ninja + MSVC)...
cmake -S "%ROOT%" -B "%ROOT%\build" -G "Ninja" ^
  -DCMAKE_BUILD_TYPE=Release
if errorlevel 1 (
  echo CMake configuration failed.
  exit /b 1
)

echo.
echo Building...
taskkill /f /im AMDOMEN.exe >nul 2>&1
cmake --build "%ROOT%\build" --config Release
if errorlevel 1 (
  echo Build failed.
  exit /b 1
)

echo.
echo Cleaning stale runtime libs...
if exist "%ROOT%\output\libs" rmdir /s /q "%ROOT%\output\libs"
if exist "%ROOT%\output\slint_cpp.dll" del /q "%ROOT%\output\slint_cpp.dll" 2>nul

echo.
echo Build successful. Binary: output\AMDOMEN.exe
exit /b 0
