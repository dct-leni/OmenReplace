@echo off
setlocal EnableExtensions

set "ROOT=%~dp0"
if "%ROOT:~-1%"=="\" set "ROOT=%ROOT:~0,-1%"

echo === OmenControl Requirements ===
echo.

:: ── 1. MSVC Build Tools ──
echo [1/2] Checking MSVC Build Tools...
set "MSVC_OK=0"

for %%P in ("%ProgramFiles%" "%ProgramFiles(x86)%") do (
  for %%E in (BuildTools Community Professional Enterprise) do (
    if exist "%%~P\Microsoft Visual Studio\2022\%%E\VC\Tools\MSVC" (
      for /d %%d in ("%%~P\Microsoft Visual Studio\2022\%%E\VC\Tools\MSVC\*") do (
        if exist "%%d\bin\Hostx64\x64\cl.exe" set "MSVC_OK=1"
      )
    )
  )
)

if "%MSVC_OK%"=="0" (
  echo MSVC Build Tools not found. Installing via winget...
  where winget >nul 2>&1
  if errorlevel 1 (
    echo ERROR: winget not found. Install App Installer from Microsoft Store first.
    echo        https://apps.microsoft.com/detail/9nblggh4nns1
    exit /b 1
  )
  winget install --id Microsoft.VisualStudio.2022.BuildTools ^
    --silent --accept-source-agreements ^
    --override "--quiet --wait --norestart --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
  if errorlevel 1 (
    echo MSVC installation failed.
    exit /b 1
  )
  echo MSVC Build Tools installed. Reboot may be required before first build.
) else (
  echo MSVC Build Tools found.
)

:: ── 2. nlohmann/json (single header) ──
echo.
echo [2/2] Checking nlohmann/json...
if not exist "%ROOT%\vendor\nlohmann\json.hpp" (
  echo Downloading nlohmann/json.hpp...
  set "PS_JSON=%TEMP%\omen-json-%RANDOM%.ps1"
  (
    echo $ErrorActionPreference = 'Stop'
    echo $OutFile = $args[0]
    echo $Dir = Split-Path -Parent $OutFile
    echo New-Item -ItemType Directory -Force -Path $Dir ^| Out-Null
    echo [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    echo $hdrs = @{ 'User-Agent' = 'OmenControl' }
    echo $rel = Invoke-RestMethod -Uri 'https://api.github.com/repos/nlohmann/json/releases/latest' -Headers $hdrs
    echo $tag = $rel.tag_name
    echo Write-Host "Downloading nlohmann/json $tag ..."
    echo $url = 'https://github.com/nlohmann/json/releases/download/' + $tag + '/json.hpp'
    echo Invoke-WebRequest -Uri $url -Headers $hdrs -OutFile $OutFile
    echo if ^(-not ^(Test-Path $OutFile^)^) { throw 'json.hpp missing after download.' }
  ) > "%PS_JSON%"

  powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%PS_JSON%" "%ROOT%\vendor\nlohmann\json.hpp"
  set "RC=%ERRORLEVEL%"
  if exist "%PS_JSON%" del /f /q "%PS_JSON%" 2>nul
  if %RC% neq 0 (
    echo nlohmann/json download failed.
    exit /b 1
  )
  echo nlohmann/json installed: %ROOT%\vendor\nlohmann\json.hpp
) else (
  echo nlohmann/json found: %ROOT%\vendor\nlohmann\json.hpp
)

:: ── 3. Clean up old toolchains ──
if exist "%ROOT%\TOOLCHAIN" (
  echo Removing old MinGW TOOLCHAIN...
  rmdir /s /q "%ROOT%\TOOLCHAIN"
)
if exist "%ROOT%\vendor\slint" (
  echo Removing old Slint SDK...
  rmdir /s /q "%ROOT%\vendor\slint"
)
if exist "%ROOT%\build" (
  echo Removing stale CMake build directory...
  rmdir /s /q "%ROOT%\build"
)
if exist "%ROOT%\vendor\imgui" (
  echo Removing old ImGui vendor directory...
  rmdir /s /q "%ROOT%\vendor\imgui"
)
if exist "%ROOT%\vendor\lhm-pawnio" (
  echo Removing old LHM PawnIO vendor directory...
  rmdir /s /q "%ROOT%\vendor\lhm-pawnio"
)
if exist "%ROOT%\ui" (
  echo Removing old Slint UI directory...
  rmdir /s /q "%ROOT%\ui"
)

echo.
echo === All requirements installed ===
echo MSVC Build Tools ready.
echo Run build.bat to compile.
exit /b 0
