# Copyright (C) 2026-2027 Ziad Mohsin. All Rights Reserved.
"""
mountxai — Official AI Package for MountX Programming Language.
"""

from mountxai.config import API_KEY, NATIVE_BIN, REPO_ROOT
from mountxai.engine import solve_task, ask_chat, correct_code, explain_code
from mountxai.catalog import STDLIB_CATALOG
from mountxai.runner import run_mountx_code

__all__ = [
    "solve_task",
    "ask_chat",
    "correct_code",
    "explain_code",
    "STDLIB_CATALOG",
    "run_mountx_code",
    "API_KEY",
    "NATIVE_BIN",
    "REPO_ROOT"
]
