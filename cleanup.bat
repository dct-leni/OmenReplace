@echo off
setlocal EnableExtensions

set "ROOT=%~dp0"
if "%ROOT:~-1%"=="\" set "ROOT=%ROOT:~0,-1%"

if exist "%ROOT%\build" rmdir /s /q "%ROOT%\build"
if exist "%ROOT%\output" rmdir /s /q "%ROOT%\output"

echo Cleanup complete.
del "%~f0"
