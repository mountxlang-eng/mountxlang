#!/usr/bin/env bash
# Copyright (C) 2026-2027 Ziad Mohsin. All Rights Reserved.
# tools/build_wasm.sh - Compiles MountX C Substrate & AOT Stdlib to WebAssembly
set -e

REPO_ROOT="$(cd -P "$(dirname "${BASH_SOURCE[0]}")/.." >/dev/null 2>&1 && pwd)"
WEB_DIR="${REPO_ROOT}/web"
mkdir -p "${WEB_DIR}"

echo "=== [Building MountX WebAssembly Target (mountx.wasm)] ==="

# 1. Locate emcc compiler
EMCC_BIN=""
if command -v emcc >/dev/null 2>&1; then
    EMCC_BIN="emcc"
elif [ -f "${REPO_ROOT}/tmp/emsdk/upstream/emscripten/emcc" ]; then
    EMCC_BIN="${REPO_ROOT}/tmp/emsdk/upstream/emscripten/emcc"
fi

if [ -z "$EMCC_BIN" ]; then
    echo "Error: emcc not found! Please ensure emsdk is installed in tmp/emsdk or PATH."
    exit 1
fi

echo "Using Emscripten compiler: $EMCC_BIN"

# 2. Compile mini.c + stdlib_aot.c + sqlite3.c with optimization and modularization
"$EMCC_BIN" \
    -O3 \
    -DMX_SUBSTRATE \
    -DMX_BOTH \
    -D__EMSCRIPTEN__ \
    -isystem "${REPO_ROOT}/gate2/g1" \
    -s WASM=1 \
    -s ALLOW_MEMORY_GROWTH=1 \
    -s MODULARIZE=1 \
    -s EXPORT_NAME="createMountX" \
    -s EXPORTED_FUNCTIONS='["_mountx_run"]' \
    -s EXPORTED_RUNTIME_METHODS='["ccall", "cwrap"]' \
    -s NO_EXIT_RUNTIME=1 \
    -s SUPPORT_LONGJMP=wasm \
    "${REPO_ROOT}/gate2/g1/mini.c" \
    "${REPO_ROOT}/gate2/g1/stdlib_aot.c" \
    "${REPO_ROOT}/gate2/g1/sqlite3.c" \
    -o "${WEB_DIR}/mountx.js"

echo "WebAssembly compilation finished successfully."
echo "Artifacts generated in ${WEB_DIR}/:"
ls -lh "${WEB_DIR}/mountx.js" "${WEB_DIR}/mountx.wasm"

# 3. Run automated Node.js test suite
NODE_BIN="$(command -v node || echo "/home/theyad75/.nvm/versions/node/v24.20.0/bin/node")"
if [ -x "$NODE_BIN" ]; then
    echo ""
    echo "Running Node.js test harness on mountx.wasm..."
    "$NODE_BIN" "${WEB_DIR}/test_wasm.js"
fi

echo "=== [MountX WebAssembly Target Built & Verified Successfully] ==="
