#!/usr/bin/env bash
# Copyright (C) 2026-2027 Ziad Mohsin. All Rights Reserved.
# tools/serve_web.sh - Launch MountX WebAssembly Playground on local HTTP server
set -e

REPO_ROOT="$(cd -P "$(dirname "${BASH_SOURCE[0]}")/.." >/dev/null 2>&1 && pwd)"
WEB_DIR="${REPO_ROOT}/web"
PORT=8088

echo "=========================================================="
echo "  🚀 MountX WebAssembly Interactive Playground"
echo "  🌐 URL: http://localhost:${PORT}"
echo "  📁 Serving directory: ${WEB_DIR}"
echo "=========================================================="

# Try to open in default browser in background
if command -v xdg-open >/dev/null 2>&1; then
    (sleep 1 && xdg-open "http://localhost:${PORT}") &
fi

python3 -m http.server "$PORT" --directory "$WEB_DIR"
