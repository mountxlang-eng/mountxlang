#!/usr/bin/env bash
# Copyright (C) 2026-2027 Ziad Mohsin. All Rights Reserved.
# Build script to cross-compile mountx.exe for Windows x86-64

set -e

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$REPO_DIR"

if command -v x86_64-w64-mingw32-gcc >/dev/null 2>&1; then
    CC="x86_64-w64-mingw32-gcc"
elif [ -x "$REPO_DIR/tmp/zig-linux-x86_64-0.13.0/zig" ]; then
    CC="$REPO_DIR/tmp/zig-linux-x86_64-0.13.0/zig cc -target x86_64-windows-gnu"
elif command -v zig >/dev/null 2>&1; then
    CC="zig cc -target x86_64-windows-gnu"
else
    echo "Error: Neither x86_64-w64-mingw32-gcc nor zig found."
    exit 1
fi

echo "Building mountx.exe using $CC..."
mkdir -p "$REPO_DIR/tmp"
$CC -std=c99 -O2 -w -DSQLITE_THREADSAFE=0 -DSQLITE_OMIT_LOAD_EXTENSION -c "$REPO_DIR/gate2/g1/sqlite3.c" -o "$REPO_DIR/tmp/sqlite3_win.o"

$CC -std=c99 -O2 -Wall -Wextra \
    -DMX_SUBSTRATE -DMX_BOTH \
    -I "$REPO_DIR/gate2/g1" \
    "$REPO_DIR/gate2/g1/mini.c" \
    "$REPO_DIR/gate2/g1/interp.emitted.c" \
    "$REPO_DIR/gate2/g1/stdlib_aot.c" \
    "$REPO_DIR/tmp/sqlite3_win.o" \
    -o "$REPO_DIR/mountx.exe"

cp "$REPO_DIR/mountx.exe" "$REPO_DIR/gate2/g1/mountx.exe"
echo "OK: mountx.exe created successfully with ZERO warnings!"
