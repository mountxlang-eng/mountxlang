@echo off
REM Copyright (C) 2026-2027 Ziad Mohsin. All Rights Reserved.
REM Build script for MountX Windows Native Executable (mountx.exe)
REM Supports both MSVC (Visual Studio) and MinGW / GCC.

echo [MountX] Building mountx.exe...

where cl >nul 2>nul
if %errorlevel% equ 0 (
    echo [MountX] Using MSVC (cl.exe)...
    cl /nologo /O2 /W3 /D_CRT_SECURE_NO_WARNINGS /DMX_SUBSTRATE /DMX_BOTH /DSQLITE_THREADSAFE=0 /DSQLITE_OMIT_LOAD_EXTENSION /I gate2\g1 gate2\g1\mini.c gate2\g1\interp.emitted.c gate2\g1\stdlib_aot.c gate2\g1\sqlite3.c /Fe:mountx.exe
    if %errorlevel% equ 0 (
        echo [MountX] Successfully built mountx.exe with MSVC!
        del *.obj 2>nul
        exit /b 0
    ) else (
        echo [MountX] MSVC build failed!
        exit /b 1
    )
)

where gcc >nul 2>nul
if %errorlevel% equ 0 (
    echo [MountX] Using MinGW (gcc.exe)...
    gcc -std=c99 -O2 -Wall -Wextra -DMX_SUBSTRATE -DMX_BOTH -DSQLITE_THREADSAFE=0 -DSQLITE_OMIT_LOAD_EXTENSION -I gate2/g1 gate2/g1/mini.c gate2/g1/interp.emitted.c gate2/g1/stdlib_aot.c gate2/g1/sqlite3.c -o mountx.exe
    if %errorlevel% equ 0 (
        echo [MountX] Successfully built mountx.exe with MinGW!
        exit /b 0
    ) else (
        echo [MountX] MinGW build failed!
        exit /b 1
    )
)

echo [MountX] Error: Neither MSVC (cl.exe) nor MinGW (gcc.exe) was found in PATH.
exit /b 1
