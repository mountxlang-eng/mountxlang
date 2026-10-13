# Copyright (C) 2026-2027 Ziad Mohsin. All Rights Reserved.
"""
mountxai/config.py — Configuration, path resolution, and API key management.
"""

import os
import sys
import re

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.dirname(SCRIPT_DIR)
if REPO_ROOT not in sys.path:
    sys.path.insert(0, REPO_ROOT)
NATIVE_BIN = os.path.join(REPO_ROOT, "mountx-native")
TMP_RUN_FILE = os.path.join(REPO_ROOT, "tmp", "agent_run.mx")
BASE_URL = "https://api.deepseek.com/chat/completions"
DEFAULT_MODEL = "deepseek-chat"
STDLIB_DIR = os.path.join(REPO_ROOT, "shared", "stdlib")

def resolve_api_key():
    """استخراج مفتاح DeepSeek من البيئة أو ملفات المفاتيح المحلية."""
    if os.environ.get("DEEPSEEK_API_KEY"):
        return os.environ.get("DEEPSEEK_API_KEY").strip()
    
    key_paths = [
        os.path.join(SCRIPT_DIR, "deepseek.txt"),
        os.path.expanduser("~/ds.py"),
        os.path.join(SCRIPT_DIR, "ds.py")
    ]
    for p in key_paths:
        if os.path.exists(p):
            try:
                with open(p, "r", encoding="utf-8") as f:
                    content = f.read()
                    match = re.search(r"sk-[a-zA-Z0-9]{32,}", content)
                    if match:
                        return match.group(0).strip()
            except Exception:
                pass
    return None

API_KEY = resolve_api_key()
