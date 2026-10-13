# Copyright (C) 2026-2027 Ziad Mohsin. All Rights Reserved.
"""
mountxai/client.py — DeepSeek API network client.
"""

import json
import urllib.request
import urllib.error
from mountxai.config import BASE_URL, API_KEY, DEFAULT_MODEL

def call_deepseek(messages, model=DEFAULT_MODEL, temperature=0.1, timeout=45):
    """استدعاء واجهة DeepSeek API بطريقة نظيفة وموثوقة."""
    if not API_KEY:
        raise RuntimeError("مفتاح DeepSeek غير موجود! تأكد من وجوده في mountxai/deepseek.txt أو متغير DEEPSEEK_API_KEY")

    payload = json.dumps({
        "model": model,
        "messages": messages,
        "temperature": temperature
    }).encode("utf-8")

    req = urllib.request.Request(
        BASE_URL,
        data=payload,
        headers={
            "Content-Type": "application/json",
            "Authorization": f"Bearer {API_KEY}"
        },
        method="POST"
    )

    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            data = json.loads(resp.read().decode("utf-8"))
            return data["choices"][0]["message"]["content"]
    except urllib.error.HTTPError as e:
        err_msg = e.read().decode("utf-8", errors="replace")
        raise RuntimeError(f"HTTP {e.code} Error from DeepSeek: {err_msg}")
    except urllib.error.URLError as e:
        raise RuntimeError(f"Network connection error: {e.reason}")
    except Exception as e:
        raise RuntimeError(f"API Error: {str(e)}")
