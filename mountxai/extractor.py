# Copyright (C) 2026-2027 Ziad Mohsin. All Rights Reserved.
"""
mountxai/extractor.py — Extract and sanitize MountX source code from LLM responses.
"""

import re

def extract_mountx_code(text):
    """استخراج كود MountX النقي مع تنظيف السياج والتعليقات بدقة."""
    if not text:
        return None

    # 1. فحص ما إذا كان الكود داخل سياج markdown
    fences = re.findall(r"```(?:mountx|mx)?\s*\n([\s\S]*?)\n```", text, re.IGNORECASE)
    candidates = fences if fences else [text]

    for candidate in candidates:
        lines = candidate.split("\n")
        code_lines = []
        inside = False

        for ln in lines:
            trimmed = ln.strip()
            if not inside:
                if trimmed == "mountx":
                    inside = True
            else:
                if trimmed == "unmountx":
                    inside = False
                    break
                # تجنب أي سياج متبقي
                if not trimmed.startswith("```"):
                    code_lines.append(ln)

        if code_lines:
            return "mountx\n" + "\n".join(code_lines).strip() + "\nunmountx\n"

    # محاولة ثانية: إذا كان داخل سياج لكن بدون وسوم mountx صريحة
    if fences:
        clean_lines = [
            ln for ln in fences[0].split("\n")
            if not ln.strip().startswith("```") and ln.strip() not in ["mountx", "unmountx"]
        ]
        if clean_lines:
            return "mountx\n" + "\n".join(clean_lines).strip() + "\nunmountx\n"

    return None
