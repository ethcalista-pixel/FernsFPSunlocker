@echo off
REM ---------------------------------------------------------------------------
REM Build Ferns Unlocker (with embedded SSR + SSAO post-process shader).
REM
REM Requires a 32-bit MinGW-w64 toolchain on PATH:
REM   i686-w64-mingw32-gcc  and  windres
REM (e.g. MSYS2: pacman -S mingw-w64-i686-gcc; then use the MINGW32 shell, or
REM  add C:\msys64\mingw32\bin to PATH).
REM
REM 32-bit is required because Ferns.exe is a 32-bit process.
REM ---------------------------------------------------------------------------
setlocal

echo [1/3] compiling shader proxy dll...
i686-w64-mingw32-gcc -shared -O2 -o dxgi.dll shader\proxy.c shader\proxy.def -ld3d11 -ldxgi -lgdi32 -luuid
if errorlevel 1 goto err

echo [2/3] embedding shader resource...
windres unlocker.rc -O coff -o unlocker_res.o
if errorlevel 1 goto err

echo [3/3] compiling unlocker...
i686-w64-mingw32-gcc -O2 -mwindows -o FernsUnlocker.exe unlocker.c unlocker_res.o -lshell32 -lgdi32
if errorlevel 1 goto err

del unlocker_res.o 2>nul
echo.
echo done -^> FernsUnlocker.exe
goto :eof

:err
echo.
echo BUILD FAILED.
exit /b 1
