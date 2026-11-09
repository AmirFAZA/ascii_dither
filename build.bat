@echo off
rem ------------------------------------------------------------------
rem  Build ascii_dither.exe (Win32 GUI + CLI) with MinGW-w64 GCC.
rem  Usage:  build.bat                 (uses gcc from PATH)
rem          build.bat C:\path\gcc.exe
rem ------------------------------------------------------------------
setlocal

set "GCC=gcc"
if not "%~1"=="" set "GCC=%~1"

if not exist dist mkdir dist

"%GCC%" -O2 -std=c99 -mwindows -static -static-libgcc -Isrc -o dist\ascii_dither.exe src\ascii_dither.c -lcomctl32 -lgdi32 -lcomdlg32 -luser32 -lshell32 -lm
if errorlevel 1 (
    echo [error] compile failed
    exit /b 1
)

rem Embed the application manifest (visual styles + DPI awareness).
where python >nul 2>nul
if errorlevel 1 (
    echo [warn] Python not found - manifest not embedded ^(classic controls^)
) else (
    python tools\embed_manifest.py dist\ascii_dither.exe res\ascii_dither.manifest
    if errorlevel 1 echo [warn] manifest embedding failed - app still works
)

echo Built: dist\ascii_dither.exe
exit /b 0
