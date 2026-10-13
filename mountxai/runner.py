# Copyright (C) 2026-2027 Ziad Mohsin. All Rights Reserved.
"""
mountxai/runner.py — Execute MountX code via the native C substrate (mountx-native).
"""

import os
import time
import subprocess
from mountxai.config import REPO_ROOT, NATIVE_BIN, TMP_RUN_FILE

def ensure_native_binary():
    """التحقق من وجود الثنائي mountx-native وتجميعه تلقائياً إن لم يكن موجوداً."""
    if not os.path.exists(NATIVE_BIN):
        subprocess.run([
            "gcc", "-std=c99", "-O2", "-Wall", "-Wextra",
            "-DMX_SUBSTRATE", "-DMX_BOTH", "-I", "gate2/g1",
            "gate2/g1/mini.c", "gate2/g1/interp.emitted.c", "gate2/g1/stdlib_aot.c",
            "tmp/sqlite3_linux.o", "-lm", "-lpthread", "-ldl",
            "-o", NATIVE_BIN
        ], cwd=REPO_ROOT, check=True)

def run_mountx_code(code_str, timeout=8, target_file=None):
    """حفظ الكود مؤقتاً وتشغيله عبر المفسر الأصلي mountx-native وقياس زمن التنفيذ."""
    run_file = target_file if target_file else TMP_RUN_FILE
    os.makedirs(os.path.dirname(run_file), exist_ok=True)
    with open(run_file, "w", encoding="utf-8") as f:
        f.write(code_str)

    ensure_native_binary()

    t_start = time.perf_counter()
    try:
        proc = subprocess.run(
            [NATIVE_BIN, run_file],
            cwd=REPO_ROOT,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            encoding="utf-8",
            errors="replace",
            timeout=timeout
        )
        elapsed_ms = (time.perf_counter() - t_start) * 1000.0
        return {
            "exit_code": proc.returncode,
            "stdout": proc.stdout.strip(),
            "stderr": proc.stderr.strip(),
            "duration_ms": elapsed_ms,
            "timed_out": False
        }
    except subprocess.TimeoutExpired:
        elapsed_ms = (time.perf_counter() - t_start) * 1000.0
        return {
            "exit_code": 124,
            "stdout": "",
            "stderr": f"Execution timed out after {timeout} seconds (Possible infinite loop).",
            "duration_ms": elapsed_ms,
            "timed_out": True
        }
    except Exception as e:
        elapsed_ms = (time.perf_counter() - t_start) * 1000.0
        return {
            "exit_code": 1,
            "stdout": "",
            "stderr": str(e),
            "duration_ms": elapsed_ms,
            "timed_out": False
        }
