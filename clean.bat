@echo off
setlocal

echo Cleaning build and output directories...

if exist build (
    echo Deleting build directory...
    rmdir /s /q build
)

if exist output (
    echo Deleting output directory...
    rmdir /s /q output
)

echo Cleanup complete.
pause
